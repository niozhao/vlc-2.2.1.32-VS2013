#ifndef _FEC_2D_PARITY_MULTIPLEXOR_HH
#define _FEC_2D_PARITY_MULTIPLEXOR_HH

#include <vector>
#include "FECMultiplexor.hh"
#include "FECPacket.hh"
#include "FECDecoder.hh"
#include "FECCluster.hh"

class ReorderingPacketBuffer;
class BufferPacket;
class BufferedPacketFactory;

class FEC2DParityMultiplexor : public FECMultiplexor {
public:
    static FEC2DParityMultiplexor* createNew(UsageEnvironment& env, u_int8_t row, u_int8_t column, long long repairWindow);
    FEC2DParityMultiplexor(UsageEnvironment& env, u_int8_t row, u_int8_t column, long long repairWindow);
    ~FEC2DParityMultiplexor();
    void pushFECRTPPacket(unsigned char* buffer, unsigned bufferSize);
	void pushFECRTPPacket(BufferedPacket* packet);
	void createReorderBuffers(BufferedPacketFactory* packetFactory);
	void setCallback(unsigned char* to, unsigned maxSize, afterGettingFunc*, void*);

public:
    Boolean first;


    std::vector<RTPPacket*> emergencyBuffer;
    std::vector<FECCluster*> superBuffer;

    u_int16_t currentSequenceNumber;
private:
    static void sendNext(void* firstArg);
    void repairPackets();

    void findBase(int* didFind, u_int16_t* newBase);
    FECCluster* findCluster(u_int16_t seqNum);
    void handleEmergencyBuffer(u_int16_t base);
    void flushCluster(RTPPacket** cluster);

    void updateCurrentSequenceNumber(u_int16_t newSeqnum, unsigned sourcePacketCount);

    void setHostSSRCIfNotSet(RTPPacket* rtpPacket);

    void setBaseIfNotSet(RTPPacket* rtpPacket);

    bool insertPacket(RTPPacket* rtpPacket);

    void printSuperBuffer();

	Boolean processFECHeader(BufferedPacket* packet);
	void resetInterFECBuffer();
	void resetNonInterFECBuffer();
	void preProcessFECPacket(bool bInterleave, BufferedPacket* srcPacket);  //true for bInterleave, false for NonInterleave

private:
    long long fRepairWindow;

    Boolean ssrcWasSet;
    unsigned hostSSRC;

    u_int8_t fRow;
    u_int8_t fColumn;

	ReorderingPacketBuffer* fInterleavedReorderingBuffer;
	ReorderingPacketBuffer* fNonInterleavedReorderingBuffer;
	Boolean fCurrentPacketBeginsFrame;
	Boolean fPacketLossInFragmentedFrame;
	Boolean fCurrentPacketCompletesFrame;

	//for Interleave
	unsigned char* fInterFECBuffer;   //拼接的完整Interleave冗余包将会放在这里 
	unsigned fInterMaxSize; //fInterFECBuffer 可用的空间大小
	unsigned char *fInterTo; // fInterTo = fInterFECBuffer + offset
	unsigned fInterFrameSize;  //Interleave冗余包的大小

	//for NonInterleave
	unsigned char* fNonInterFECBuffer;
	unsigned fNonInterMaxSize; // in
	unsigned char *fNonInterTo; // in
	unsigned fNonInterFrameSize; // out

};
#endif
