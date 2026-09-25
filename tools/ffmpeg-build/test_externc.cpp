#include <libavformat/avformat.h>
int test_ref(AVFormatContext** p, const char* s) { return avformat_open_input(p, s, nullptr, nullptr); }
