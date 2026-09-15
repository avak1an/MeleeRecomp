# Copies the freshly built launcher into pc/dist without failing the build
# when the destination cannot be written, which happens when that copy is
# the running launcher that started the build (it renames itself first, but
# a launcher started from elsewhere, or an antivirus scan, can still hold it).
#   cmake -DSRC=<built exe> -DDST=<pc/dist/melee-launcher.exe> -P copy_launcher.cmake
file(COPY_FILE "${SRC}" "${DST}" RESULT result ONLY_IF_DIFFERENT)
if(result)
    message(WARNING
        "The launcher was built but could not be copied to ${DST} (${result}). "
        "It is probably running: close it and rebuild, or copy build/pc/melee-launcher.exe there yourself.")
endif()
