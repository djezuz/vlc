/*****************************************************************************
 * es_out_managed.c
 *****************************************************************************
 *
 * Authors: Rui Zhang <bbcallen _AT_ gmail _DOT_ com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_es_out_managed.h>
#include <vlc_demux.h>
#include <assert.h>

struct es_out_sys_t
{
    demux_t         *p_demux;           /* weak reference, for log */
    es_out_t        *p_backend_out;     /* weak reference */

    vlc_array_t     *p_es_fmt_cache;    /* fmt cache array */

    bool            b_discontinuity;
};

typedef struct
{
    es_out_id_t *p_es_id;   /* weak reference */
    es_format_t fmt;        /* need free */
} es_out_fmt_entry;

static es_out_id_t *EsFmtCacheFind( es_out_t *p_out, const es_format_t *p_fmt )
{
    es_out_sys_t *p_sys = p_out->p_sys;

    vlc_array_t *p_es_fmt_cache = p_sys->p_es_fmt_cache;
    for( int i = 0; i < vlc_array_count( p_sys->p_es_fmt_cache ); ++i )
    {
        es_out_fmt_entry *entry = (es_out_fmt_entry *) vlc_array_item_at_index( p_es_fmt_cache, i );
        if( es_format_IsSimilar( &entry->fmt, p_fmt ) )
            return entry->p_es_id;
    }

    return NULL;
}

static void EsFmtCacheAdd( es_out_t *p_out, es_out_id_t* p_es_id, const es_format_t *p_fmt )
{
    es_out_sys_t *p_sys = p_out->p_sys;

    assert( !EsFmtCacheFind( p_out, p_fmt ) );

    es_out_fmt_entry *entry = (es_out_fmt_entry *) malloc( sizeof(es_out_fmt_entry) );
    if( entry == NULL )
        return;
    memset( entry, 0, sizeof(es_out_fmt_entry) );

    entry->p_es_id = p_es_id;
    es_format_Copy(&entry->fmt, p_fmt);

    vlc_array_append( p_sys->p_es_fmt_cache , entry );
}

static es_out_id_t *EsOutAdd( es_out_t *p_out, const es_format_t *p_fmt )
{
    assert( p_out );
    es_out_id_t *p_es_id = EsFmtCacheFind( p_out, p_fmt );
    if( p_es_id )
    {
        msg_Info( p_out->p_sys->p_demux, "EsOutAdd reuse" );
        return p_es_id;
    }
    else
    {
        msg_Info( p_out->p_sys->p_demux, "EsOutAdd for the 1st time" );
    }

    p_es_id = es_out_Add( p_out->p_sys->p_backend_out, p_fmt );
    if( p_es_id )
        EsFmtCacheAdd( p_out, p_es_id, p_fmt );

    return p_es_id;
}

static int EsOutSend( es_out_t *p_out, es_out_id_t *p_es, block_t *p_block )
{
    assert( p_out );
    if( p_out->p_sys->b_discontinuity )
    {
        p_out->p_sys->b_discontinuity = false;
        es_out_Control( p_out->p_sys->p_backend_out, ES_OUT_RESET_PCR );
    }

    return es_out_Send( p_out->p_sys->p_backend_out, p_es, p_block );
}

static void EsOutDel( es_out_t *p_out, es_out_id_t *p_es )
{
    assert( p_out );
    es_out_Del( p_out->p_sys->p_backend_out, p_es );
}

static int EsOutControl( es_out_t *p_out, int i_query, va_list args )
{
    assert( p_out );
    if( ES_OUT_POST_DISCONTINUITY == i_query )
    {
        p_out->p_sys->b_discontinuity = true;
        return VLC_SUCCESS;
    }

    return es_out_vaControl( p_out->p_sys->p_backend_out, i_query, args );
}

static void EsOutDestroy( es_out_t *p_out )
{
    if( !p_out )
        return;

    vlc_array_t *p_es_fmt_cache = p_out->p_sys->p_es_fmt_cache;
    if( p_es_fmt_cache )
    {
        int count = vlc_array_count( p_es_fmt_cache );
        for( int i = 0; i < count; ++i )
        {
            es_out_fmt_entry* p_entry = (es_out_fmt_entry *) vlc_array_item_at_index( p_es_fmt_cache, i );
            if( !p_entry )
                continue;

            es_format_Clean( &p_entry->fmt );
            free( p_entry );
        }

        vlc_array_destroy( p_es_fmt_cache );
    }

    free( p_out->p_sys );
    free( p_out );
}

es_out_t *demux_EsOutManagedNew( demux_t *p_demux, es_out_t *p_backend_es_out )
{
    es_out_t *p_out = malloc( sizeof(*p_out) );
    memset( p_out, 0, sizeof(*p_out) );

    es_out_sys_t *p_sys;

    if( !p_out )
        return NULL;

    p_out->pf_add     = EsOutAdd;
    p_out->pf_send    = EsOutSend;
    p_out->pf_del     = EsOutDel;
    p_out->pf_control = EsOutControl;
    p_out->pf_destroy = EsOutDestroy;

    p_out->p_sys = p_sys = malloc( sizeof(*p_sys) );
    if( !p_sys )
    {
        free( p_out );
        return NULL;
    }
    memset( p_sys, 0, sizeof(*p_sys) );

    p_sys->p_es_fmt_cache = vlc_array_new();
    p_sys->p_demux = p_demux;
    p_sys->p_backend_out = p_backend_es_out;

    return p_out;
}