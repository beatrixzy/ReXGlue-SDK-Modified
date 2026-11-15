static const AVCodecParser* const parser_list[] = {
#if CONFIG_FLAC_PARSER
    &ff_flac_parser,
#endif
#if CONFIG_MPEGAUDIO_PARSER
    &ff_mpegaudio_parser,
#endif
#if CONFIG_VORBIS_PARSER
    &ff_vorbis_parser,
#endif
    NULL};
