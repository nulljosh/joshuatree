#ifndef PMM_H
#define PMM_H
void pmm_init(unsigned int multiboot_info_addr);
unsigned int pmm_alloc_frame(void);   /* physical addr of a free 4K frame, or 0 */
void pmm_free_frame(unsigned int addr);
unsigned int pmm_total_frames(void);
unsigned int pmm_free_frames(void);
#endif
