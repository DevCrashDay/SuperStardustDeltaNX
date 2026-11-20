/* gxp_precompile.h: every GXP the game has -- on disk or as it registers it -- to a SPIR-V dump set, once -- see gxp_precompile.c */
#ifndef GXP_PRECOMPILE_H
#define GXP_PRECOMPILE_H
void gxp_precompile_maybe(void);       /* RECOMP_PRECOMPILE_SPV=<dir>: the set lands there, then the game boots as usual */
void gxp_precompile_register(uint32_t prog_addr);   /* the registration hook's half: a program the game just loaded (its bytes real only now) */
#endif
