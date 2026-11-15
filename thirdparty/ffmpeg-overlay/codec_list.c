static const AVCodec* const codec_list[] = {
#if CONFIG_APE_DECODER
    &ff_ape_decoder,
#endif
#if CONFIG_DSD_LSBF_DECODER
    &ff_dsd_lsbf_decoder,
#endif
#if CONFIG_DSD_MSBF_DECODER
    &ff_dsd_msbf_decoder,
#endif
#if CONFIG_DSD_LSBF_PLANAR_DECODER
    &ff_dsd_lsbf_planar_decoder,
#endif
#if CONFIG_DSD_MSBF_PLANAR_DECODER
    &ff_dsd_msbf_planar_decoder,
#endif
#if CONFIG_FLAC_DECODER
    &ff_flac_decoder,
#endif
#if CONFIG_MP3FLOAT_DECODER
    &ff_mp3float_decoder,
#endif
#if CONFIG_MP3_DECODER
    &ff_mp3_decoder,
#endif
#if CONFIG_PCM_F32LE_DECODER
    &ff_pcm_f32le_decoder,
#endif
#if CONFIG_PCM_F64LE_DECODER
    &ff_pcm_f64le_decoder,
#endif
#if CONFIG_PCM_S16LE_DECODER
    &ff_pcm_s16le_decoder,
#endif
#if CONFIG_PCM_S24LE_DECODER
    &ff_pcm_s24le_decoder,
#endif
#if CONFIG_PCM_S32LE_DECODER
    &ff_pcm_s32le_decoder,
#endif
#if CONFIG_PCM_U8_DECODER
    &ff_pcm_u8_decoder,
#endif
#if CONFIG_TTA_DECODER
    &ff_tta_decoder,
#endif
#if CONFIG_VORBIS_DECODER
    &ff_vorbis_decoder,
#endif
#if CONFIG_WMAPRO_DECODER
    &ff_wmapro_decoder,
#endif
#if CONFIG_WMAV2_DECODER
    &ff_wmav2_decoder,
#endif
#if CONFIG_XMAFRAMES_DECODER
    &ff_xmaframes_decoder,
#endif
    NULL};
