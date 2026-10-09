# isotp-c (vendored)

ISO 15765-2 (ISO-TP) transport used by the CAN bootloader on class 7.

- Upstream: https://github.com/SimonCahill/isotp-c (maintained fork of lishen2/isotp-c)
- Commit: `1fc19e2f7e934c9fdf278ed1b87f32b9d983d513`
- License: MIT (see `LICENSE`)
- Files copied unmodified: `isotp.c`, `isotp.h`, `isotp_config.h`, `isotp_defines.h`,
  `isotp_user.h`.

Build configuration lives in `firmware/bootloader_stm32/CMakeLists.txt`, not in these files:
`ISO_TP_USER_SEND_CAN_ARG` (per-link driver context), `ISO_TP_FRAME_PADDING`,
`ISO_TP_NO_FORMATTED_ERRORS` and `NDEBUG` (keeps `snprintf`/`assert` out of the 32 KB
bootloader). The shim functions `isotp_user_send_can`, `isotp_user_get_us` and
`isotp_user_debug` are implemented in `src/anv_bl.c`.

To update: copy the same five files from a newer upstream commit and update the hash above.
