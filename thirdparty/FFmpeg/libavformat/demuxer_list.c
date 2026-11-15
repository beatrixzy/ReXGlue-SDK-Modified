#include "config.h"

// ASF and MP3 for title playlists; FLAC, Ogg, WAV, APE, TTA and DSD (DSF / DFF)
// for custom soundtracks
// (rexglue xmp_custom.cpp probes a player's own files against this list).
static const AVInputFormat * const demuxer_list[] = {
#if CONFIG_APE_DEMUXER
    &ff_ape_demuxer,
#endif
    &ff_asf_demuxer,
#if CONFIG_DSF_DEMUXER
    &ff_dsf_demuxer,
#endif
#if CONFIG_FLAC_DEMUXER
    &ff_flac_demuxer,
#endif
#if CONFIG_IFF_DEMUXER
    &ff_iff_demuxer,
#endif
    &ff_mp3_demuxer,
#if CONFIG_OGG_DEMUXER
    &ff_ogg_demuxer,
#endif
#if CONFIG_WAV_DEMUXER
    &ff_wav_demuxer,
#endif
#if CONFIG_TTA_DEMUXER
    &ff_tta_demuxer,
#endif
    NULL };
