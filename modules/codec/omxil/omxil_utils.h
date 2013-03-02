/*****************************************************************************
 * omxil_utils.h: helper functions
 *****************************************************************************
 * Copyright (C) 2010 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Gildas Bazin <gbazin@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * OMX macros
 *****************************************************************************/
#ifdef __ANDROID__
#define OMX_VERSION_MAJOR 1
#define OMX_VERSION_MINOR 0
#define OMX_VERSION_REV   0
#define OMX_VERSION_STEP  0
#else
#define OMX_VERSION_MAJOR 1
#define OMX_VERSION_MINOR 1
#define OMX_VERSION_REV   1
#define OMX_VERSION_STEP  0
#endif

#define OMX_INIT_COMMON(a) \
  (a).nSize = sizeof(a); \
  (a).nVersion.s.nVersionMajor = OMX_VERSION_MAJOR; \
  (a).nVersion.s.nVersionMinor = OMX_VERSION_MINOR; \
  (a).nVersion.s.nRevision = OMX_VERSION_REV; \
  (a).nVersion.s.nStep = OMX_VERSION_STEP

#define OMX_INIT_STRUCTURE(a) \
  memset(&(a), 0, sizeof(a)); \
  OMX_INIT_COMMON(a)

#define OMX_ComponentRoleEnum(hComponent, cRole, nIndex) \
    ((OMX_COMPONENTTYPE*)hComponent)->ComponentRoleEnum ? \
    ((OMX_COMPONENTTYPE*)hComponent)->ComponentRoleEnum(  \
        hComponent, cRole, nIndex ) : OMX_ErrorNotImplemented

#define CHECK_ERROR(a, ...) \
    if(a != OMX_ErrorNone) {msg_Dbg( p_dec, __VA_ARGS__ ); goto error;}

/*****************************************************************************
 * OMX buffer FIFO macros
 *****************************************************************************/
#define OMX_FIFO_PEEK(p_fifo, p_buffer) \
         p_buffer = (p_fifo)->p_first;

#define OMX_FIFO_GET(p_fifo, p_buffer) \
    do { vlc_mutex_lock( &(p_fifo)->lock ); \
         while( !(p_fifo)->p_first ) \
             vlc_cond_wait( &(p_fifo)->wait, &(p_fifo)->lock ); \
         p_buffer = (p_fifo)->p_first; \
         OMX_BUFFERHEADERTYPE **pp_next = (OMX_BUFFERHEADERTYPE **) \
             ((void **)p_buffer + (p_fifo)->offset); \
         (p_fifo)->p_first = *pp_next; *pp_next = 0; \
         if( !(p_fifo)->p_first ) (p_fifo)->pp_last = &(p_fifo)->p_first; \
         vlc_mutex_unlock( &(p_fifo)->lock ); } while(0)

#define OMX_FIFO_GET_TIMEOUT(p_fifo, p_buffer, timeout) \
    do { vlc_mutex_lock( &(p_fifo)->lock ); \
         mtime_t end = mdate() + timeout; \
         if( !(p_fifo)->p_first ) \
             vlc_cond_timedwait( &(p_fifo)->wait, &(p_fifo)->lock, end ); \
         p_buffer = (p_fifo)->p_first; \
         if( p_buffer ) { \
             OMX_BUFFERHEADERTYPE **pp_next = (OMX_BUFFERHEADERTYPE **) \
                 ((void **)p_buffer + (p_fifo)->offset); \
             (p_fifo)->p_first = *pp_next; *pp_next = 0; \
             if( !(p_fifo)->p_first ) (p_fifo)->pp_last = &(p_fifo)->p_first; \
         } \
         vlc_mutex_unlock( &(p_fifo)->lock ); } while(0)

#define OMX_FIFO_PUT(p_fifo, p_buffer) \
    do { vlc_mutex_lock (&(p_fifo)->lock);              \
         OMX_BUFFERHEADERTYPE **pp_next = (OMX_BUFFERHEADERTYPE **) \
             ((void **)p_buffer + (p_fifo)->offset); \
         *(p_fifo)->pp_last = p_buffer; \
         (p_fifo)->pp_last = pp_next; *pp_next = 0; \
         vlc_cond_signal( &(p_fifo)->wait ); \
         vlc_mutex_unlock( &(p_fifo)->lock ); } while(0)

/*****************************************************************************
 * OMX format parameters
 *****************************************************************************/
typedef union {
    OMX_PARAM_U32TYPE common;
    OMX_AUDIO_PARAM_PCMMODETYPE pcm;
    OMX_AUDIO_PARAM_MP3TYPE mp3;
    OMX_AUDIO_PARAM_AACPROFILETYPE aac;
    OMX_AUDIO_PARAM_VORBISTYPE vorbis;
    OMX_AUDIO_PARAM_WMATYPE wma;
    OMX_AUDIO_PARAM_RATYPE ra;
    OMX_AUDIO_PARAM_ADPCMTYPE adpcm;
    OMX_AUDIO_PARAM_G723TYPE g723;
    OMX_AUDIO_PARAM_G726TYPE g726;
    OMX_AUDIO_PARAM_G729TYPE g729;
    OMX_AUDIO_PARAM_AMRTYPE amr;

    OMX_VIDEO_PARAM_H263TYPE h263;
    OMX_VIDEO_PARAM_MPEG2TYPE mpeg2;
    OMX_VIDEO_PARAM_MPEG4TYPE mpeg4;
    OMX_VIDEO_PARAM_WMVTYPE wmv;
    OMX_VIDEO_PARAM_RVTYPE rv;
    OMX_VIDEO_PARAM_AVCTYPE avc;

} OmxFormatParam;

/*****************************************************************************
 * Events utility functions
 *****************************************************************************/
typedef struct OmxEvent
{
    OMX_EVENTTYPE event;
    OMX_U32 data_1;
    OMX_U32 data_2;
    OMX_PTR event_data;

    struct OmxEvent *next;
} OmxEvent;

OMX_ERRORTYPE PostOmxEvent(decoder_t *p_dec, OMX_EVENTTYPE event,
    OMX_U32 data_1, OMX_U32 data_2, OMX_PTR event_data);
OMX_ERRORTYPE WaitForOmxEvent(decoder_t *p_dec, OMX_EVENTTYPE *event,
    OMX_U32 *data_1, OMX_U32 *data_2, OMX_PTR *event_data);
OMX_ERRORTYPE WaitForSpecificOmxEvent(decoder_t *p_dec,
    OMX_EVENTTYPE specific_event, OMX_U32 *data_1, OMX_U32 *data_2,
    OMX_PTR *event_data);

/*****************************************************************************
 * Picture utility functions
 *****************************************************************************/
void CopyOmxPicture( int i_color_format, picture_t *p_pic,
                     int i_slice_height,
                     int i_src_stride, uint8_t *p_src, int i_chroma_div );

void CopyVlcPicture( decoder_t *, OMX_BUFFERHEADERTYPE *, picture_t * );

int IgnoreOmxDecoderPadding(const char *psz_name);

/*****************************************************************************
 * Logging utility functions
 *****************************************************************************/
const char *StateToString(OMX_STATETYPE state);
const char *CommandToString(OMX_COMMANDTYPE command);
const char *EventToString(OMX_EVENTTYPE event);
const char *ErrorToString(OMX_ERRORTYPE error);

void PrintOmx(decoder_t *p_dec, OMX_HANDLETYPE omx_handle, OMX_U32 i_port);

/*****************************************************************************
 * fourcc -> omx id mapping
 *****************************************************************************/
int GetOmxVideoFormat( vlc_fourcc_t i_fourcc,
                       OMX_VIDEO_CODINGTYPE *pi_omx_codec,
                       const char **ppsz_name );
int GetVlcVideoFormat( OMX_VIDEO_CODINGTYPE i_omx_codec,
                       vlc_fourcc_t *pi_fourcc, const char **ppsz_name );
int GetOmxAudioFormat( vlc_fourcc_t i_fourcc,
                       OMX_AUDIO_CODINGTYPE *pi_omx_codec,
                       const char **ppsz_name );
int OmxToVlcAudioFormat( OMX_AUDIO_CODINGTYPE i_omx_codec,
                       vlc_fourcc_t *pi_fourcc, const char **ppsz_name );
const char *GetOmxRole( vlc_fourcc_t i_fourcc, int i_cat, bool b_enc );
int GetOmxChromaFormat( vlc_fourcc_t i_fourcc,
                        OMX_COLOR_FORMATTYPE *pi_omx_codec,
                        const char **ppsz_name );
int GetVlcChromaFormat( OMX_COLOR_FORMATTYPE i_omx_codec,
                        vlc_fourcc_t *pi_fourcc, const char **ppsz_name );
int GetVlcChromaSizes( vlc_fourcc_t i_fourcc,
                       unsigned int width, unsigned int height,
                       unsigned int *size, unsigned int *pitch,
                       unsigned int *chroma_pitch_div );

/*****************************************************************************
 * Functions to deal with audio format parameters
 *****************************************************************************/
OMX_ERRORTYPE SetAudioParameters(OMX_HANDLETYPE handle,
    OmxFormatParam *param, OMX_U32 i_port, OMX_AUDIO_CODINGTYPE encoding,
    vlc_fourcc_t i_codec, uint8_t i_channels, unsigned int i_samplerate,
    unsigned int i_bitrate, unsigned int i_bps, unsigned int i_blocksize);
OMX_ERRORTYPE GetAudioParameters(OMX_HANDLETYPE handle,
    OmxFormatParam *param, OMX_U32 i_port, OMX_AUDIO_CODINGTYPE encoding,
    uint8_t *pi_channels, unsigned int *pi_samplerate,
    unsigned int *pi_bitrate, unsigned int *pi_bps, unsigned int *pi_blocksize);
unsigned int GetAudioParamSize(OMX_INDEXTYPE index);

/*****************************************************************************
 * Vendor specific color formats
 *****************************************************************************/
#define OMX_QCOM_COLOR_FormatYVU420SemiPlanar 0x7FA30C00
#define OMX_TI_COLOR_FormatYUV420PackedSemiPlanar 0x7F000100
#define QOMX_COLOR_FormatYUV420PackedSemiPlanar64x32Tile2m8ka 0x7FA30C03
#define OMX_IndexVendorSetYUV420pMode 0x7f000003

/*****************************************************************************
 * H264 profile level numbers
 *****************************************************************************/
#define H264_PROFILE_BASELINE   0x42
#define H264_PROFILE_MAIN       0x4d
#define H264_PROFILE_EXTENDED   0x58
#define H264_PROFILE_HIGH       0x64
#define H264_PROFILE_HIGH_10    0x6e
#define H264_PROFILE_HIGH_422   0x7a
#define H264_PROFILE_HIGH_444   0xf4

/* Return "unknown" for unknown profile */
const char *H264ProfileToString(int profile_id);
/* Return "unknown" for unknown profile */
const char *OmxProfileTypeToString(OMX_VIDEO_AVCPROFILETYPE omx_profile);
/* Return OMX_VIDEO_AVCProfileMax for unknown level */
OMX_VIDEO_AVCPROFILETYPE H264ProfileToOmxType(int profile_id);

/* Return 0 for unknown level */
uint8_t OmxLevelTypeToH264Level(OMX_VIDEO_AVCLEVELTYPE omx_profile);
/* Return OMX_VIDEO_AVCLevelMax for unknown level */
OMX_VIDEO_AVCLEVELTYPE H264LevelToOmxType(int level);