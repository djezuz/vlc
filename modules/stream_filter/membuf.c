/*****************************************************************************
 * membuf.c: Stream buffer in memory
 *****************************************************************************
 *
 * Authors: Rui Zhang <bbcallen _AT_ gmail _DOT_ com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>

#include <assert.h>
#include <errno.h>

#include <vlc_threads.h>
#include <vlc_arrays.h>
#include <vlc_stream.h>

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

#define ENABLE_TEXT N_("")
#define ENABLE_LONGTEXT N_("")

vlc_module_begin ()
    set_description (N_("Memory stream buffer"))
    set_category (CAT_INPUT)
    set_subcategory (SUBCAT_INPUT_STREAM_FILTER)
    set_capability ("stream_filter", 1)
    add_shortcut( "membuf" )
    add_bool( "membuf-enable", false, ENABLE_TEXT, ENABLE_LONGTEXT, false )
    set_callbacks (Open, Close)
vlc_module_end ()

/*****************************************************************************
 * Documentation
 *
 * Read / Peek
 *      while( !buffered_enough )
 *      {
 *          wait_data_or_eof_or_abort;
 *      }
 *      return buffered_data;
 *
 * Control STREAM_GET_PREBUFFER_FINISHED
 *      return buffered_enough;
 *
 *****************************************************************************/

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/

#define BUFFER_BLOCK_SIZE ( 4 * 1024 * 1024 )
#define BYTES_PER_READ ( 16 * 1024 )
#define SHORT_SEEK_RANGE ( 64 * 1024 )

typedef struct {
    uint8_t    *p_buffer;       /* allocated data */
    int         i_block_size;   /* supposed not chaged */

    /*
     * NOTE:
     *  Only buffer thread can write data or modify range, so we don't need lock
     *  when we fetch data to unbuffered area.
     */
    vlc_mutex_t range_lock;         /* lock [i_data_begin, i_data_end] */

    volatile int i_data_begin;  /* buffered data begin byte, may not be 0, if seeked */
    volatile int i_data_end;    /* buffered data end byte, not greater than i_capacity */
} buffer_block_t;

struct stream_sys_t
{
    uint64_t    i_stream_size;
    bool        b_can_fastseek;
    bool        b_can_seek;
    bool        b_error;        /* should return error from Read/Peek/Control */
    bool        b_close;        /* should exit prebuffer thread */

    vlc_mutex_t wait_fill_lock;
    vlc_cond_t  wait_fill;

    vlc_mutex_t wait_rewind_lock;
    vlc_cond_t  wait_rewind;

    /*
     * NOTE:
     *  Buffer between [i_stream_offset, i_prebuffer_offset) should be always valid.
     *  So, we don't need to lock block data read/write
     *
     *  If stream is seeked into or back over unbuffered data,
     *  i_prebuffer_offset should be recaculated immediately.
     *
     *  i_prebuffer_offset > source_lock > buffer_block_t.range_lock
     *
     */
    vlc_mutex_t source_lock;        /* lock stream->p_source */
    vlc_array_t block_array;        /* all blocks, elements can be NULL, if not fetched */
    volatile uint64_t i_stream_offset;

    vlc_mutex_t prebuffer_offset_lock;      /* lock i_prebuffer_offset, b_buffered_eos */
    volatile uint64_t i_prebuffer_offset;   /* first unbuffered byte after i_stream_offset */
    volatile bool     b_buffered_eos;       /* buffered to the end */

    vlc_thread_t prebuffer_thread;

    /* Temporary buffer for Peek() */
    uint8_t    *p_temp_peek;
    int         i_temp_peek_capacity;
};

static int Read   ( stream_t *p_stream, void *p_buffer, unsigned int i_read );
static int Peek   ( stream_t *p_stream, const uint8_t **pp_peek, unsigned int i_peek );
static int Control( stream_t *p_stream, int i_query, va_list args );

static void* PrebufferThread(void *);

static stream_sys_t *sys_Alloc( void );
static void sys_Free( stream_sys_t* p_sys);

#define msg_VVV msg_Err

/****************************************************************************
 * Open/Close
 ****************************************************************************/

static int Open( vlc_object_t *obj )
{
    stream_t *p_stream = (stream_t *)obj;
    stream_sys_t *p_sys = NULL;

    assert( p_stream );
    assert( p_stream->p_source );

    if( !p_stream->p_source )
        goto EXIT_FAIL;

    /* FIXME: Need a better way to check if already loaded in stream filter chain */
    if( p_stream->p_source->p_source )
        goto EXIT_FAIL;

    bool b_enabled = var_InheritBool( p_stream, "membuf-enable" );
    if( !b_enabled )
    {
        msg_Info( p_stream, "membuf: disable membuf" );
        goto EXIT_FAIL;
    }

    p_sys = sys_Alloc();
    if( !p_sys )
        goto EXIT_FAIL;

    /* get stream information */
    p_sys->i_stream_size = stream_Size( p_stream->p_source );
    if( p_sys->i_stream_size <= 0 )
    {
        msg_Err( p_stream, "membuf: stream unknown size" );
        goto EXIT_FAIL;
    }
    msg_Info( p_stream, "membuf: stream size: %"PRId64, p_sys->i_stream_size );

    stream_Control( p_stream->p_source, STREAM_CAN_FASTSEEK, &p_sys->b_can_fastseek );
    stream_Control( p_stream->p_source, STREAM_CAN_SEEK, &p_sys->b_can_seek );

    /* */
    p_stream->p_sys = p_sys;
    p_stream->pf_read = Read;
    p_stream->pf_peek = Peek;
    p_stream->pf_control = Control;

    if(VLC_SUCCESS != vlc_clone( &p_sys->prebuffer_thread, PrebufferThread, p_stream, VLC_THREAD_PRIORITY_INPUT ) )
        goto EXIT_FAIL;

    msg_VVV( p_stream, "membuf: loaded" );
    return VLC_SUCCESS;
EXIT_FAIL:
    sys_Free( p_sys );

    return VLC_EGENERIC;
}

static void Close( vlc_object_t *obj )
{
    stream_t *p_stream = (stream_t *)obj;
    stream_sys_t *p_sys = p_stream->p_sys;

    if( p_sys )
    {
        /* wakeup thread */
        vlc_mutex_lock( &p_sys->wait_rewind_lock );
        mutex_cleanup_push( &p_sys->wait_rewind_lock );
        vlc_cond_signal( &p_sys->wait_rewind );
        vlc_cleanup_run( );

        vlc_cancel( p_sys->prebuffer_thread );
        vlc_join( p_sys->prebuffer_thread, NULL );
    }

    sys_Free( p_sys );
}

/****************************************************************************
 * Alloc/Free
 ****************************************************************************/
static buffer_block_t *buffer_block_Alloc( )
{
    buffer_block_t *p_block = (buffer_block_t *) malloc( sizeof(buffer_block_t) );
    if( !p_block )
        return NULL;
    memset( p_block, 0, sizeof(buffer_block_t) );

    p_block->p_buffer = (uint8_t *) malloc( BUFFER_BLOCK_SIZE );
    if( !p_block->p_buffer )
    {
        free( p_block );
        return NULL;
    }
    memset( p_block->p_buffer, 0, BUFFER_BLOCK_SIZE );

    vlc_mutex_init( &p_block->range_lock );
    p_block->i_block_size = BUFFER_BLOCK_SIZE;

    return p_block;
}

static void buffer_block_Free( buffer_block_t *p_block )
{
    free( p_block->p_buffer );

    vlc_mutex_destroy( &p_block->range_lock );
    p_block->p_buffer = NULL;

    free( p_block );
}

static void peek_Free( stream_sys_t* p_sys )
{
    if( p_sys->p_temp_peek )
    {
        free( p_sys->p_temp_peek );
        p_sys->p_temp_peek = NULL;
        p_sys->i_temp_peek_capacity = 0;
    }
}

static uint8_t *peek_Alloc( stream_sys_t* p_sys, int i_size )
{
    if( i_size <= p_sys->i_temp_peek_capacity )
    {
        assert( p_sys->p_temp_peek );
        return p_sys->p_temp_peek;
    }

    peek_Free( p_sys );

    p_sys->p_temp_peek = (uint8_t *) malloc( i_size );
    if( !p_sys->p_temp_peek )
        return NULL;

    p_sys->i_temp_peek_capacity = i_size;
    return p_sys->p_temp_peek;
}

static stream_sys_t *sys_Alloc( void )
{
    stream_sys_t *p_sys = (stream_sys_t *) malloc( sizeof(stream_sys_t) );
    if( !p_sys )
        return NULL;
    memset( p_sys, 0, sizeof(stream_sys_t) );

    vlc_mutex_init( &p_sys->source_lock );
    vlc_mutex_init( &p_sys->prebuffer_offset_lock );

    vlc_mutex_init( &p_sys->wait_fill_lock );
    vlc_cond_init( &p_sys->wait_fill );

    vlc_mutex_init( &p_sys->wait_rewind_lock );
    vlc_cond_init( &p_sys->wait_rewind );

    return p_sys;
}

static void sys_Free( stream_sys_t* p_sys )
{
    if( p_sys )
    {
        if( p_sys->p_temp_peek )
        {
            free( p_sys->p_temp_peek );
            p_sys->p_temp_peek = NULL;
            p_sys->i_temp_peek_capacity = 0;
        }

        int count = vlc_array_count( &p_sys->block_array );
        for( int i = 0; i < count; ++i )
        {
            buffer_block_t *p_block = vlc_array_item_at_index( &p_sys->block_array, i );
            if( p_block )
                buffer_block_Free( p_block );
        }

        vlc_mutex_destroy( &p_sys->source_lock );
        vlc_mutex_destroy( &p_sys->prebuffer_offset_lock );

        vlc_mutex_destroy( &p_sys->wait_fill_lock );
        vlc_cond_destroy( &p_sys->wait_fill );

        vlc_mutex_destroy( &p_sys->wait_rewind_lock );
        vlc_cond_destroy( &p_sys->wait_rewind );

        free( p_sys );
    }
}


static uint64_t unsafe_FindRewindBufferedPosition( stream_t *p_stream, uint64_t i_start_pos )
{
    stream_sys_t *p_sys = p_stream->p_sys;
    int i_block_index = i_start_pos / BUFFER_BLOCK_SIZE;
    int i_block_offset = i_start_pos % BUFFER_BLOCK_SIZE;
    uint64_t i_rewind_pos = i_start_pos;

    int i_array_count = vlc_array_count( &p_sys->block_array );
    if( i_block_index >= i_array_count )
        return i_rewind_pos;

    /* data is buffered at start point */
    while( i_block_index < i_array_count )
    {
        buffer_block_t *p_block = (buffer_block_t *) vlc_array_item_at_index( &p_sys->block_array, i_block_index );
        if( !p_block )
            return i_rewind_pos;

        /* uncompleted at head of block */
        if( i_block_offset < p_block->i_data_begin )
            return i_rewind_pos;

        /* uncompleted at tail of block */
        if( i_block_offset >= p_block->i_data_end )
            return i_rewind_pos;

        /* rewind to end of this block */
        i_rewind_pos = (uint64_t) i_block_index * BUFFER_BLOCK_SIZE + p_block->i_data_end;

        /* unfinished or EOS block */
        if( p_block->i_data_end < BUFFER_BLOCK_SIZE )
            return i_rewind_pos;

        i_block_offset = 0;
        ++i_block_index;
    }

    return i_rewind_pos;
}

static uint64_t safe_GetPrebufferOffset( stream_t *p_stream )
{
    stream_sys_t *p_sys = p_stream->p_sys;
    uint64_t i_prebuffer_offset = 0;

    vlc_mutex_lock( &p_sys->prebuffer_offset_lock );
    mutex_cleanup_push( &p_sys->prebuffer_offset_lock );

    i_prebuffer_offset = p_sys->i_prebuffer_offset;

    vlc_cleanup_run();

    return i_prebuffer_offset;
}

/****************************************************************************
 * Prebuffer Thread
 *
 * try to be cancellation-safe
 ****************************************************************************/
static void* PrebufferThread(void *p_this)
{
    stream_t *p_stream = (stream_t *) p_this;
    stream_sys_t *p_sys = p_stream->p_sys;

    while( true )
    {
        uint64_t i_prebuffer_offset = safe_GetPrebufferOffset( p_stream );
        while( i_prebuffer_offset >= p_sys->i_stream_size )
        {
            msg_Info( p_stream, "membuf: EOS, wait for seek or exit" );

            if( p_sys->b_error || p_sys->b_close )
                goto EXIT_THREAD;


            /* Mark as EOS, and wake up any waiting Read/Peek */
            vlc_mutex_lock( &p_sys->prebuffer_offset_lock );
            mutex_cleanup_push( &p_sys->prebuffer_offset_lock );
            p_sys->b_buffered_eos = true;
            vlc_cleanup_run( );

            vlc_mutex_lock( &p_sys->wait_fill_lock );
            mutex_cleanup_push( &p_sys->wait_fill_lock );
            vlc_cond_signal( &p_sys->wait_fill );
            vlc_cleanup_run( );

            /* Wait for seek or exit */
            vlc_mutex_lock( &p_sys->wait_rewind_lock );
            mutex_cleanup_push( &p_sys->wait_rewind_lock );
            vlc_cond_wait( &p_sys->wait_rewind, &p_sys->wait_rewind_lock );
            vlc_cleanup_run( );

            /* get latest prebuffer offset */
            i_prebuffer_offset = safe_GetPrebufferOffset( p_stream );
        }

        /**
         * find block to prebuffer
         * rewind if necessary
         */
        int i_block_index = i_prebuffer_offset / BUFFER_BLOCK_SIZE;
        int i_block_offset = i_prebuffer_offset % BUFFER_BLOCK_SIZE;
        buffer_block_t *p_block = NULL;
        {
            vlc_mutex_lock( &p_sys->prebuffer_offset_lock );
            mutex_cleanup_push( &p_sys->prebuffer_offset_lock );

            /* grow array if need */
            // msg_VVV( p_stream, "grow array if need" );
            while( i_block_index >= vlc_array_count( &p_sys->block_array ) )
            {
                vlc_array_append( &p_sys->block_array, NULL );
            }

            /* alloc block if need */
            // msg_VVV( p_stream, "alloc block" );
            p_block = vlc_array_item_at_index( &p_sys->block_array, i_block_index );
            if( !p_block )
            {
                p_block = buffer_block_Alloc();
                if( p_block )
                {
                    /* calculate block size */
                    if( 0 == p_sys->i_stream_size )
                    {
                        p_block->i_block_size = 0;
                    }
                    else
                    {
                        int i_last_block_index = ( p_sys->i_stream_size - 1 ) / BUFFER_BLOCK_SIZE;
                        if( i_block_index == i_last_block_index )
                        {
                            p_block->i_block_size = ( p_sys->i_stream_size - 1 ) % BUFFER_BLOCK_SIZE + 1;
                        }
                        else
                        {
                            p_block->i_block_size = BUFFER_BLOCK_SIZE;
                        }
                    }

                    // msg_VVV( p_stream, "vlc_array_set" );
                    vlc_array_set( &p_sys->block_array, p_block, i_block_index );
                }
                else
                {
                    // msg_Err( p_stream, "membuf: buffer_block_Alloc() failed" );
                    p_sys->b_error = true;
                }
            }

            if( p_block )
            {
                vlc_mutex_lock( &p_block->range_lock );
                mutex_cleanup_push( &p_block->range_lock );

                // msg_VVV( p_stream, "prepare block" );
                if( i_block_offset < p_block->i_data_begin )
                {
                    /*
                     * uncompleted at head of block,
                     * drop buffered data, rewind to request position.
                     */
                    p_block->i_data_begin = i_block_offset;
                    p_block->i_data_end = i_block_offset;
                }
                else if( i_block_offset >= p_block->i_data_end )
                {
                    /*
                     * uncompleted at tail of block,
                     * drop buffered data, rewind to request position.
                     */
                    p_block->i_data_end = i_block_offset;
                }
                else if( i_block_offset < p_block->i_data_end )
                {
                    /*
                     * FIXME:
                     *  we have some buffered segment data,
                     *  but we need stream_Seek() to rewind,
                     *  FOR NOW,
                     *  drop buffered data, rewind to request position.
                     *
                     * TODO:
                     *  check if worth stream_Seek();
                     */
                    p_block->i_data_end = i_block_offset;
                }
                vlc_cleanup_run( ); /* block_lock */

            } /* end if( p_block ) */
            vlc_cleanup_run( ); /* prebuffer_offset_lock */

            if( p_sys->b_error || p_sys->b_close )
                goto EXIT_THREAD;
        } /* end { */

        /* start fill block */
        // msg_VVV( p_stream, "start fill block %d => %d", (int) i_block_offset, (int) p_block->i_block_size );
        while( i_block_offset < p_block->i_block_size )
        {
            if( p_sys->b_error || p_sys->b_close )
                goto EXIT_THREAD;

            int i_left_size = p_block->i_block_size - i_block_offset;
            int i_step_read = BYTES_PER_READ;
            if( i_step_read > i_left_size )
                i_step_read = i_left_size;

            /* write on unbuffered area, no need to lock block */
            int i_read_ret = 0;
            bool b_need_rewind = false;

            uint64_t i_latest_prebuffer_offset = safe_GetPrebufferOffset( p_stream );
            if( i_prebuffer_offset == i_latest_prebuffer_offset )
            {
                /*
                 * p_sys->i_prebuffer_offset could be changed while we are reading from source stream.
                 * it will be checked again after reading finished.
                 */
                {
                    vlc_mutex_lock( &p_sys->source_lock );
                    mutex_cleanup_push( &p_sys->source_lock );

                    uint64_t i_source_offset = stream_Tell( p_stream->p_source );
                    if( i_prebuffer_offset == i_source_offset )
                    {
                        i_read_ret = stream_Read( p_stream->p_source,
                                                  p_block->p_buffer + i_block_offset,
                                                  i_step_read );
                    }
                    else
                    {
                        msg_Err( p_stream, "membuf: wrong prebuffer offset, expected: %"PRId64", actual: %"PRId64,
                                 i_prebuffer_offset,
                                 i_source_offset );
                        b_need_rewind = true;
                    }

                    vlc_cleanup_run( );
                }

                /*
                msg_VVV( p_stream, "PrebufferThread: stream_Read %"PRId64" at block[%d][%d]+%d (%d read)",
                        p_sys->i_prebuffer_offset,
                        i_block_index,
                        i_block_offset,
                        i_step_read,
                        i_read_ret );
                */

                if( i_read_ret > 0 )
                {
                    vlc_mutex_lock( &p_sys->prebuffer_offset_lock );
                    mutex_cleanup_push( &p_sys->prebuffer_offset_lock );

                    /* move block cursor */
                    vlc_mutex_lock( &p_block->range_lock );
                    mutex_cleanup_push( &p_block->range_lock );

                    i_block_offset += i_read_ret;
                    p_block->i_data_end += i_read_ret;
                    assert( p_block->i_data_end <= p_block->i_block_size );

                    vlc_cleanup_run( ); /* block lock */

                    uint64_t i_latest_prebuffer_offset = p_sys->i_prebuffer_offset;
                    if( i_prebuffer_offset == i_latest_prebuffer_offset )
                    {
                        i_prebuffer_offset += i_read_ret;
                        p_sys->i_prebuffer_offset = i_prebuffer_offset;
                    }
                    else
                    {
                        b_need_rewind = true;
                        msg_Err( p_stream, "membuf: prebuffer offset was changed while we are reading" );
                    }

                    vlc_cleanup_run( ); /* prebuffer offset lock */
                }
            }
            else
            {
                b_need_rewind = true;
                msg_Err( p_stream, "membuf: prebuffer offset was changed while we are looking for block" );
            }

            if( b_need_rewind )
            {
                /* seek happened, we need to change prebuffer position */
                break;
            }
            else if( i_read_ret <= 0 )
            {
                p_sys->b_error = true;
                goto EXIT_THREAD;
            }

            /* wakeup blocked Read/Peek */
            vlc_mutex_lock( &p_sys->wait_fill_lock );
            mutex_cleanup_push( &p_sys->wait_fill_lock );

            vlc_cond_signal( &p_sys->wait_fill );

            vlc_cleanup_run( );
        } /* end while( i_block_offset < p_block->i_block_size ) */
    } /* end while( true ) */

EXIT_THREAD:

    if( p_sys->b_error ) {
        /* wakeup blocked Read/Peek */
        vlc_mutex_lock( &p_sys->wait_fill_lock );
        mutex_cleanup_push( &p_sys->wait_fill_lock );

        vlc_cond_signal( &p_sys->wait_fill );

        vlc_cleanup_run( );
    }

    msg_Info( p_stream, "membuf: PrebufferThread exit" );
    return NULL;
}

/****************************************************************************
 * Read/Peek/Control
 ****************************************************************************/

/* data must be buffered enough */
static int unsafe_fetchData( stream_t *p_stream, void *buffer, unsigned int i_read )
{
    stream_sys_t *p_sys = p_stream->p_sys;

    assert( p_sys->i_stream_offset + i_read <= p_sys->i_prebuffer_offset );

    vlc_array_t *p_block_array = &p_sys->block_array;
    int i_block_index = p_sys->i_stream_offset / BUFFER_BLOCK_SIZE;
    int i_block_offset = p_sys->i_stream_offset % BUFFER_BLOCK_SIZE;
    int i_block_count = vlc_array_count( p_block_array );
    assert( i_block_index < i_block_count );

    unsigned int i_read_offset = 0;
    unsigned int i_left_size = i_read;
    while( i_block_index < i_block_count )
    {
        buffer_block_t *p_block = (buffer_block_t *) vlc_array_item_at_index( p_block_array, i_block_index );
        assert( p_block );

        vlc_mutex_lock( &p_block->range_lock );
        mutex_cleanup_push( &p_block->range_lock );

        assert( i_block_offset >= p_block->i_data_begin );
        assert( i_block_offset < p_block->i_data_end );
        assert( p_block->i_data_end <= p_block->i_block_size );

        unsigned int i_block_left_size = p_block->i_data_end - i_block_offset;
        unsigned int i_step_read = i_left_size;
        if( i_step_read > i_block_left_size )
            i_step_read = i_block_left_size;

        memcpy( (uint8_t*) buffer + i_read_offset, p_block->p_buffer + i_block_offset, i_step_read );

        i_read_offset += i_step_read;
        i_left_size -= i_step_read;

        ++i_block_index;
        i_block_offset = 0;

        vlc_cleanup_run();

        if( i_left_size <= 0 )
            break;
    }

    return i_read_offset;
}

static int safe_WaitFillData( stream_t *p_stream, unsigned int i_read )
{
    stream_sys_t *p_sys = p_stream->p_sys;

    /* two-step volatile access */
    if( p_sys->b_buffered_eos )
    {
        vlc_mutex_lock( &p_sys->prebuffer_offset_lock );
        mutex_cleanup_push( &p_sys->prebuffer_offset_lock );
        if( p_sys->b_buffered_eos )
        {
            if( p_sys->i_stream_offset >= p_sys->i_prebuffer_offset )
            {
                i_read = 0;
            }
            else
            {
                int64_t i_left_size = p_sys->i_prebuffer_offset - p_sys->i_stream_offset;
                if( i_read > i_left_size )
                    i_read = i_left_size;
            }
        }
        vlc_cleanup_run( );
    }

    if( i_read == 0 )
        return i_read;

    /* p_sys->i_prebuffer_offset can be increased only in prebuffer thread */
    /* so, it's safe to rely on it */
    uint64_t i_prebuffer_offset = safe_GetPrebufferOffset( p_stream );
    if( p_sys->i_stream_offset + i_read <= i_prebuffer_offset )
        return i_read;

    msg_Warn( p_stream, "membuf: wait fill data %d", i_read );

    /* p_sys->i_prebuffer_offset may forward, so get a snapshot here */
    while( p_sys->i_stream_offset + i_read > i_prebuffer_offset )
    {
        if( p_sys->b_error || p_sys->b_close )
            break;

        if( p_sys->b_buffered_eos )
        {
            int64_t i_filled_len = i_prebuffer_offset - p_sys->i_stream_offset;
            if( i_read > i_filled_len )
            {
                msg_Warn( p_stream, "membuf: buffered eos before enough data filled" );
                i_read = i_filled_len;
            }
            break;
        }

        vlc_mutex_lock( &p_sys->wait_fill_lock );
        mutex_cleanup_push( &p_sys->wait_fill_lock );

        vlc_cond_wait( &p_sys->wait_fill, &p_sys->wait_fill_lock );

        vlc_cleanup_run( );

        i_prebuffer_offset = safe_GetPrebufferOffset( p_stream );
    }

    msg_Warn( p_stream, "membuf: wait fill data end %"PRId64", %"PRId64,
             p_sys->i_stream_offset,
             i_prebuffer_offset );

    if( p_sys->b_error || p_sys->b_close )
        return -1;

    return i_read;
}

static int Read( stream_t *p_stream, void *p_buffer, unsigned int i_read )
{
    // msg_VVV( p_stream, "membuf:Peek %"PRId64", %d", stream_Tell( p_stream->p_source ), i_read );
    // return stream_Read( p_stream->p_source, p_buffer, i_read );
    stream_sys_t *p_sys = p_stream->p_sys;
    // msg_VVV( p_stream, "membuf:Read %"PRId64", %d", p_sys->i_stream_offset, i_read );

    assert( p_stream->p_source );

    int i_ready_data = safe_WaitFillData( p_stream, i_read );
    if( i_ready_data <= 0 )
    {
        msg_Warn( p_stream, "membuf: Read() interrupted or eos" );
        return i_ready_data;
    }

    assert( p_sys->i_stream_offset + i_ready_data <= p_sys->i_prebuffer_offset );

    if( !p_buffer )
    {
        p_sys->i_stream_offset += i_ready_data;
        return i_ready_data;
    }

    int i_fetch = unsafe_fetchData( p_stream, p_buffer, i_ready_data );
    // msg_VVV( p_stream, "membuf:Read return %d", i_fetch );
    if( i_fetch <= 0 )
        return i_fetch;

    p_sys->i_stream_offset += i_fetch;
    return i_fetch;
}

static int Peek( stream_t *p_stream, const uint8_t **pp_peek, unsigned int i_peek )
{
    // msg_VVV( p_stream, "membuf:Peek %"PRId64", %d", stream_Tell( p_stream->p_source ), i_peek );
    // return stream_Peek( p_stream->p_source, pp_peek, i_peek );
    stream_sys_t *p_sys = p_stream->p_sys;
    // msg_VVV( p_stream, "membuf:Peek %"PRId64", %d", p_sys->i_stream_offset, i_peek );

    assert( p_stream->p_source );

    int i_ready_data = safe_WaitFillData( p_stream, i_peek );
    if( i_ready_data <= 0 )
    {
        msg_Warn( p_stream, "membuf: Peek() interrupted or eos" );
        return i_ready_data;
    }

    assert( p_sys->i_stream_offset + i_ready_data <= p_sys->i_prebuffer_offset );

    if( !pp_peek )
        return i_ready_data;

    int i_block_index = p_sys->i_stream_offset / BUFFER_BLOCK_SIZE;
    int i_block_offset = p_sys->i_stream_offset % BUFFER_BLOCK_SIZE;
    if( i_block_offset + i_ready_data <= BUFFER_BLOCK_SIZE )
    {
        /* in same block */
        buffer_block_t *p_block = (buffer_block_t *) vlc_array_item_at_index( &p_sys->block_array, i_block_index );
        assert( p_block );
        assert( i_block_offset >= p_block->i_data_begin );
        assert( i_block_offset < p_block->i_data_end );
        assert( p_block->i_data_end <= p_block->i_block_size );

        /*
        msg_VVV( p_stream, "peek in same block[%d][%d]+%d (%x~%x)",
                 i_block_index, i_block_offset, i_peek,
                 (int) ( p_block->p_buffer[i_block_offset] ),
                 (int) ( p_block->p_buffer[i_block_offset + i_ready_data - 1] ));
        */
        *pp_peek = p_block->p_buffer + i_block_offset;
        return i_peek;
    }

    uint8_t *p_temp_peek = peek_Alloc( p_sys, i_ready_data );
    if( !p_temp_peek )
        return -1;

    *pp_peek = p_temp_peek;
    return unsafe_fetchData( p_stream, p_temp_peek, i_ready_data );
}

static int Control( stream_t *p_stream, int i_query, va_list args )
{
    stream_sys_t *p_sys = p_stream->p_sys;

    assert( p_stream->p_source );
    switch (i_query)
    {
        case STREAM_CAN_FASTSEEK:
        {
            //return stream_vaControl( p_stream->p_source, i_query, args );
            *(va_arg (args, bool *)) = p_sys->b_can_fastseek;
            return VLC_SUCCESS;
        }
        case STREAM_CAN_SEEK:
        {
            //return stream_vaControl( p_stream->p_source, i_query, args );
            *(va_arg (args, bool *)) = p_sys->b_can_seek;
            return VLC_SUCCESS;
        }
        case STREAM_GET_POSITION:
        {
            //return stream_vaControl( p_stream->p_source, i_query, args );
            *(va_arg (args, uint64_t *)) = p_sys->i_stream_offset;
            return VLC_SUCCESS;
        }
        case STREAM_SET_POSITION:
        {
            //return stream_vaControl( p_stream->p_source, i_query, args );
            if( !p_sys->b_can_seek )
                return VLC_EGENERIC;

            int i_seek_ret = VLC_EGENERIC;
            uint64_t i_seek_pos = (va_arg (args, uint64_t));
            msg_VVV( p_stream, "membuf: seek to %"PRId64, i_seek_pos );

            /* short seek */
            {
                uint64_t i_prebuffer_offset = safe_GetPrebufferOffset( p_stream );
                uint64_t i_stream_offset = p_sys->i_stream_offset;
                if( i_seek_pos > i_prebuffer_offset && i_seek_pos < i_prebuffer_offset + SHORT_SEEK_RANGE)
                {
                    msg_VVV( p_stream, "membuf: short seek out of buffered range ~%"PRId64" (expected %"PRId64")",
                            i_prebuffer_offset,
                            i_seek_pos );

                    int i_read = i_seek_pos - i_stream_offset;
                    int i_ready_data = safe_WaitFillData( p_stream, i_read );
                    if( i_ready_data <= 0 )
                    {
                        msg_Warn( p_stream, "membuf: Read() interrupted or eos" );
                        return i_ready_data;
                    }
                }
            }

            vlc_mutex_lock( &p_sys->prebuffer_offset_lock );
            mutex_cleanup_push( &p_sys->prebuffer_offset_lock );

            uint64_t i_prebuffer_rewind_pos = unsafe_FindRewindBufferedPosition( p_stream, i_seek_pos );

            /* check current position */
            if( i_seek_pos <= p_sys->i_prebuffer_offset &&
                i_seek_pos < i_prebuffer_rewind_pos )
            {
                msg_VVV( p_stream, "membuf: seek within buffered range ~%"PRId64" (expected %"PRId64")",
                        i_prebuffer_rewind_pos,
                        p_sys->i_prebuffer_offset );

                p_sys->i_stream_offset = i_seek_pos;
                i_seek_ret = VLC_SUCCESS;
            }
            else
            {
                uint64_t i_rewind_offset = 0;
                /* seek does not occur too often, just lock all we need to make life easy */
                vlc_mutex_lock( &p_sys->source_lock );
                mutex_cleanup_push( &p_sys->source_lock );

                msg_VVV( p_stream, "membuf: seek out of buffered range, rewind to %"PRId64, i_prebuffer_rewind_pos );
                i_seek_ret = stream_Seek( p_stream->p_source, i_prebuffer_rewind_pos );

                /* no matter fail or success, we rely on stream_Tell() */
                i_rewind_offset = stream_Tell( p_stream->p_source );
                msg_VVV( p_stream, "membuf: seek rewind end at %"PRId64, i_rewind_offset );

                vlc_cleanup_run( ); /* source lock */

                p_sys->b_buffered_eos = false;
                p_sys->i_prebuffer_offset = i_rewind_offset;

                if( i_seek_pos <= i_rewind_offset )
                {
                    /* reasonable seek result */
                    p_sys->i_stream_offset = i_seek_pos;
                }
                else if( p_sys->i_stream_offset > i_rewind_offset )
                {
                    /* may be failed to seek, do not modify play offset */
                    p_sys->i_stream_offset = i_rewind_offset;
                    i_seek_ret = VLC_EGENERIC;
                }
            }

            vlc_cleanup_run( ); /* prebuffer offset lock */

            /* wakeup prebuffer thread */
            vlc_mutex_lock( &p_sys->wait_rewind_lock );
            mutex_cleanup_push( &p_sys->wait_rewind_lock );
            vlc_cond_signal( &p_sys->wait_rewind );
            vlc_cleanup_run( );

            return i_seek_ret;
        }
        case STREAM_GET_SIZE:
        {
            //return stream_vaControl( p_stream->p_source, i_query, args );
            *(va_arg (args, uint64_t *)) = p_sys->i_stream_size;
            return VLC_SUCCESS;
        }
        case STREAM_GET_CACHED_SIZE:
        {
            /* not critical data, no need to lock */
            *(va_arg (args, uint64_t *)) = p_sys->i_prebuffer_offset;
            return VLC_SUCCESS;
        }
        default:
        {
            //return stream_vaControl( p_stream->p_source, i_query, args );
            return VLC_EGENERIC;
        }
    }
}