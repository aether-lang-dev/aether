# System libraries a hand-written C link line needs, by platform.
#
# glibc keeps dlopen/dlsym in libdl and shm_open/clock_* in librt. The BSDs
# (FreeBSD, NetBSD, OpenBSD) and macOS have them in libc, and the linker
# there fails outright on `-ldl` / `-lrt` ("unable to find library -ldl").
# A test that links against build/libaether.a by hand uses these instead of
# spelling the Linux libraries.
#
#   cc ... build/libaether.a -lpthread $AE_DL_LIBS -lm $AE_RT_LIBS ...
#
# Unquoted on purpose: empty means no word at all.
case "$(uname -s)" in
    Linux) AE_DL_LIBS="-ldl"; AE_RT_LIBS="-lrt" ;;
    *)     AE_DL_LIBS="";     AE_RT_LIBS="" ;;
esac
