#ifndef SH2_DYNAREC_H
#define SH2_DYNAREC_H

#define SH2CORE_DYNAREC 2

void sh2_dynarec_init(void);
int verify_dirty(pointer addr);
void invalidate_all_pages(void);

void YabauseDynarecOneFrameExec(int, int);
void DynarecMapDataArrays(void);

/* Writes to VDP1 RAM (r=0) or VDP2 VRAM (r=1) when code has been compiled
   from there (see sh2_dynarec.c) */
extern int vram_shadow_any;
void SH2DynarecVramWrite(int r, u32 addr, u32 size);
#define SH2_DYNAREC_VRAM_WRITE(r, addr, size) \
   do { if (UNLIKELY(vram_shadow_any)) SH2DynarecVramWrite(r, addr, size); } while (0)

#endif
