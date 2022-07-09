# NOTE : libvosk.so does not have a SONAME field set.
# We must set one with if we want to link it properly :
# patchelf --set-soname libvosk.so libvosk.so

install_data('libvosk.so', install_dir : get_option('libdir'))
