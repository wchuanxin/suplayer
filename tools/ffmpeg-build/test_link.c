#include <libavformat/avformat.h>
#include <stdio.h>
int main(void) { printf("%s\n", avformat_configuration()); return 0; }
