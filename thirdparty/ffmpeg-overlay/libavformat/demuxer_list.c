#include "libavformat/avformat.h"

extern AVInputFormat ff_asf_demuxer;
extern AVInputFormat ff_flac_demuxer;
extern AVInputFormat ff_mp3_demuxer;
extern AVInputFormat ff_ogg_demuxer;
extern AVInputFormat ff_wav_demuxer;
extern AVInputFormat ff_ape_demuxer;
extern AVInputFormat ff_dsf_demuxer;
extern AVInputFormat ff_iff_demuxer;
extern AVInputFormat ff_tta_demuxer;

// ASF and MP3 for title playlists; FLAC, Ogg, WAV, APE, TTA and DSD (DSF / DFF)
// for custom soundtracks
// (xmp_custom.cpp probes a player's own files against this list).
static const AVInputFormat * const demuxer_list[] = {
#if CONFIG_APE_DEMUXER
    &ff_ape_demuxer,
#endif
    &ff_asf_demuxer,
#if CONFIG_DSF_DEMUXER
    &ff_dsf_demuxer,
#endif
    &ff_flac_demuxer,
#if CONFIG_IFF_DEMUXER
    &ff_iff_demuxer,
#endif
    &ff_mp3_demuxer,
    &ff_ogg_demuxer,
    &ff_wav_demuxer,
#if CONFIG_TTA_DEMUXER
    &ff_tta_demuxer,
#endif
    NULL
};
