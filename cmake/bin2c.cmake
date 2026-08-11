# Converts a binary file (IN) into a C source (OUT) defining
#   unsigned char g_<SYM>[]; unsigned long long g_<SYM>_size;
# SYM defaults to link_glb. Used by the self-contained `zelda_packed`
# target to embed the models and audio.
if(NOT DEFINED SYM)
    set(SYM link_glb)
endif()
file(READ "${IN}" hex_data HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," c_bytes "${hex_data}")
file(WRITE "${OUT}"
     "/* generated from ${IN} -- do not edit */\n"
     "unsigned char g_${SYM}[] = {${c_bytes}};\n"
     "unsigned long long g_${SYM}_size = sizeof(g_${SYM});\n")
