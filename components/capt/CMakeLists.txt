# Canon CAPT laser printers: protocol engine and Hi-SCoA compression (portable C;
# fake_capt.c, the emulated printer, is for the host tests only).
idf_component_register(SRCS "capt.c" "hiscoa.c"
                       INCLUDE_DIRS "include")
# hot loops: the encoder's match search
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
