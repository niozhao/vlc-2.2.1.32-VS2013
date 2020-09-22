#include "include/RTPPacket.hh"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <Windows.h>

void DebugPrintf(const char* format, ...)
{
	char strBuffer[4096] = { 0 };
	va_list vlArgs;
	va_start(vlArgs, format);
	_vsnprintf(strBuffer, sizeof(strBuffer) - 1, format, vlArgs);
	va_end(vlArgs);
	OutputDebugStringA(strBuffer);
}

std::string FormatString(const char* format, ...)
{
	char strBuffer[1024] = { 0 };
	va_list vlArgs;
	va_start(vlArgs, format);
	_vsnprintf(strBuffer, sizeof(strBuffer) - 1, format, vlArgs);
	va_end(vlArgs);
	return (std::string)strBuffer;
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
