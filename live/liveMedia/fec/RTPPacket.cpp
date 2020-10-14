#include "include/RTPPacket.hh"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef WIN32
#include <Windows.h>
#elif __ANDROID__
#include <android/log.h>
#define  LOG_TAG    "FEC"
#endif

void DebugPrintf(const char* format, ...)
{
	va_list vlArgs;
	va_start(vlArgs, format);
#ifdef WIN32
	char strBuffer[4096] = { 0 };
	_vsnprintf(strBuffer, sizeof(strBuffer) - 1, format, vlArgs);
	OutputDebugStringA(strBuffer);
#elif __ANDROID__
	__android_log_vprint(ANDROID_LOG_INFO, LOG_TAG, format, vlArgs);
#endif
	va_end(vlArgs);
}

char const* FormatString(const char* format, ...)
{
	const int bufferSize = 1024;
	char* strBuffer = new char[bufferSize];
	memset(strBuffer, 0, bufferSize);
	va_list vlArgs;
	va_start(vlArgs, format);
#ifdef WIN32
	_vsnprintf(strBuffer, bufferSize - 1, format, vlArgs);
#elif __ANDROID__
	snprintf(strBuffer, bufferSize - 1, format, vlArgs);
#endif
	va_end(vlArgs);
	return strBuffer;
}


//TODO: Handle extension header?
RTPPacket* RTPPacket::createNew(unsigned char* rtpPacket, unsigned rtpPacketSize) {
    return new RTPPacket(rtpPacket, rtpPacketSize);
}

RTPPacket::RTPPacket(unsigned char* content, unsigned size) {
    fContent = new unsigned char[size];
	memmove(fContent, content, size);
    fSize = size;
}

RTPPacket::~RTPPacket() {
    delete[] fContent;
	fContent = NULL;
}
