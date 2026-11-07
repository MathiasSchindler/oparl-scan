#if defined(__APPLE__)
#define SYMBOL(name) _##name
.section __TEXT,__const
.private_extern _search_pdf_preview_script
.private_extern _search_pdf_renderer_script
#elif defined(_WIN32)
#define SYMBOL(name) name
.section .rdata,"dr"
#else
#define SYMBOL(name) name
.section .rodata
#endif

.globl SYMBOL(search_pdf_preview_script)
SYMBOL(search_pdf_preview_script):
.incbin "src/pdf-preview.js"
.byte 0

.globl SYMBOL(search_pdf_renderer_script)
SYMBOL(search_pdf_renderer_script):
.incbin "vendor/pdf-reader/dist/pdf-crumb.min.js"
.byte 0

#if defined(__linux__)
.section .note.GNU-stack,"",@progbits
#endif
