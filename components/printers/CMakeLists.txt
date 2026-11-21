# Printer drivers (printer_driver.h): one source file per printer family.
idf_component_register(SRCS "printers.c" "canon_pixma.c" "canon_capt.c"
                       INCLUDE_DIRS "include"
                       REQUIRES airprint canon_bjl capt)
