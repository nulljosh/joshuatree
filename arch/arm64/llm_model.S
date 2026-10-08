/* The language model's checkpoint and tokenizer (llm.c), baked into the image when the build is given their paths:
   make LLM_MODEL=/path/model.bin LLM_TOK=/path/tokenizer.bin. Without them both are empty and `llm` says so.
   On a Pi the firmware loads kernel8.img, model included, from the SD card's FAT32 boot partition. */
    .section .rodata.llm, "a"
    .balign 16
    .global llm_model_start, llm_model_end, llm_tok_start, llm_tok_end
llm_model_start:
#ifdef LLM_MODEL
    .incbin LLM_MODEL
#endif
llm_model_end:
    .balign 16
llm_tok_start:
#ifdef LLM_TOK
    .incbin LLM_TOK
#endif
llm_tok_end:
