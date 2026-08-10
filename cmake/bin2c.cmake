# Converts a binary file (IN) into a C source (OUT) defining
#   unsigned char g_link_glb[]; unsigned long long g_link_glb_size;
# Used by the self-contained `zelda_packed` target to embed the model.
file(READ "${IN}" hex_data HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," c_bytes "${hex_data}")
file(WRITE "${OUT}"
     "/* generated from ${IN} -- do not edit */\n"
     "unsigned char g_link_glb[] = {${c_bytes}};\n"
     "unsigned long long g_link_glb_size = sizeof(g_link_glb);\n")
