#include "include/FEC2DParityMultiplexor.hh"
#include "GroupsockHelper.hh"
#include "FECEncoder.hh"
#include <iostream>
#define MAX_FEC_BUFFER_SIZE (100 * 1024)

#define InterleaveIndex 0
#define NonInterleaveIndex 1

FEC2DParityMultiplexor* FEC2DParityMultiplexor::createNew(UsageEnvironment& env, u_int8_t row, u_int8_t column, long long repairWindow) {
    return new FEC2DParityMultiplexor(env, row, column, repairWindow);
}

FEC2DParityMultiplexor::FEC2DParityMultiplexor(UsageEnvironment& env, u_int8_t row, u_int8_t column, long long repairWindow) : FECMultiplexor(env){
    first = True;
    ssrcWasSet = False;
    fRepairWindow = repairWindow;
    fRow = row;
    fColumn = column;
    hostSSRC = 0;
	reordingBuffers[InterleaveIndex] = NULL;
	reordingBuffers[NonInterleaveIndex] = NULL;

	fFECBuffers[InterleaveIndex] = new unsigned char[MAX_FEC_BUFFER_SIZE];
	fFECBuffers[NonInterleaveIndex] = new unsigned char[MAX_FEC_BUFFER_SIZE];
	resetFECBuffer(InterleaveIndex);
	resetFECBuffer(NonInterleaveIndex);

    nextTask() = envir().taskScheduler().scheduleDelayedTask(20000, (TaskFunc*)sendNext, this);
}

void FEC2DParityMultiplexor::resetFECBuffer(int index)
{
	memset(fFECBuffers[index], 0, sizeof(fFECBuffers[index]));
	pTo[index] = fFECBuffers[index];
	frameSize[index] = 0;
	maxSize[index] = MAX_FEC_BUFFER_SIZE;
	currentPacketBeginsFrame[index] = True;
	currentPacketCompletesFrame[index] = True;
	packetLossInFragmentedFrame[index] = False;
}

void FEC2DParityMultiplexor::createReorderBuffers(BufferedPacketFactory* packetFactory)
{
	reordingBuffers[InterleaveIndex] = new ReorderingPacketBuffer(NULL);
	reordingBuffers[NonInterleaveIndex] = new ReorderingPacketBuffer(NULL);
}

void FEC2DParityMultiplexor::setCallback(unsigned char* to, unsigned maxSize, afterGettingFunc* callBack, void* callBackData)
{
	fReadyTo = to;
	fReadyMaxSize = maxSize;
	packetReadyCallback = callBack;
	fCallbackClientData = callBackData;
}

FEC2DParityMultiplexor::~FEC2DParityMultiplexor() {
	delete[] fFECBuffers[InterleaveIndex];
	delete[] fFECBuffers[NonInterleaveIndex];
	delete reordingBuffers[InterleaveIndex];
	delete reordingBuffers[NonInterleaveIndex];

	while (!fRTPPackets.empty()){
		RTPPacket* rtpPacket = fRTPPackets.front();
		fRTPPackets.pop();
		delete rtpPacket;
	}
	/*for (size_t i = 0; i < emergencyBuffer.size(); i++){
		delete emergencyBuffer[i];
	}
	emergencyBuffer.clear();*/
}

Boolean FEC2DParityMultiplexor::processFECHeader(BufferedPacket* packet, Boolean* startFlag, Boolean* endFlag)
{
	unsigned char* headerStart = packet->data();
	unsigned packetSize = packet->dataSize();
    
	if (packetSize < 12 + 16)
		return False;

	//unsigned char startBit = headerStart[12 + 12] & 0x80;  //Ox80: 1000 0000
	//unsigned char endBit = headerStart[12 + 12] & 0x40;  //0x40: 0100 0000

	unsigned char startBit = headerStart[12 + 12] & 0x01;  
	unsigned char endBit = headerStart[12 + 12] & 0x02;  

	*startFlag = (startBit != 0);
	*endFlag = (endBit != 0);

	if (!*startFlag)
		packet->skip(12 + 16);

	return True;
}

/******************************
  *  preProcessFECPacket()
  *
  * 目的：FEC冗余包可能分了多个RTP包发送过来，在这里接受它们，并把它们拼成完整的冗余包，之后送入后面的repair逻辑
  *
  * fCurrentPacketBeginsFrame:  标志当前RTP包是一个冗余包的起始片
  *
  * fCurrentPacketCompletesFrame： 标志当前RTP包是一个冗余包的最后一片
  *
  * 拼完整包的方法：以Interleaved为例
  * 将第一片RTP包数据拷贝至fFECBuffers[index]，offset = 0；
  * 将第二片RTP包数据拷贝至fFECBuffers[index]，offset = sizeof（第一个RTP包）
  * ...拷贝最后一个包
  *
  *
*******************************/
void FEC2DParityMultiplexor::preProcessFECPacket(int index, BufferedPacket* srcPacket)
{
	if (index > 1)
		return;
	ReorderingPacketBuffer* currentReorderBuffer = reordingBuffers[index];
	unsigned char* fCurrentFECBuffer = fFECBuffers[index];
	unsigned char **fCurrentTo = &pTo[index];
	unsigned* fCurrentFrameSize = &frameSize[index];
	unsigned* fCurrentMaxSize = &maxSize[index];
	Boolean* pCurrentPacketBeginsFrame = &currentPacketBeginsFrame[index];
	Boolean* pCurrentPacketLossInFragmentedFrame = &packetLossInFragmentedFrame[index];
	Boolean* pCurrentPacketCompletesFrame = &currentPacketCompletesFrame[index];


	BufferedPacket* bPacket = currentReorderBuffer->getFreePacket(NULL);
	bPacket->reset();
	bPacket->appendData(srcPacket->data(), srcPacket->dataSize());

	unsigned rtpHdr = ntohl(*(u_int32_t*)(bPacket->data()));
	unsigned char rtpPayloadType = (unsigned char)((rtpHdr & 0x007F0000) >> 16);
	unsigned short rtpSeqNo = (unsigned short)(rtpHdr & 0xFFFF);
	struct timeval timeNow;
	gettimeofday(&timeNow, NULL);
	bPacket->assignMiscParams(rtpSeqNo, 0, timeNow, 0, 0, timeNow);  //这里只关心seqNo及currentTime。

	bool bSucceed = currentReorderBuffer->storePacket(bPacket);
	if (!bSucceed)
		currentReorderBuffer->freePacket(bPacket);
	else
	{
		bool fNeedDelivery = true;
		while (fNeedDelivery) {
			Boolean packetLossPrecededThis;  //packetLossPrecededThis 表示什么意思？若为true表示在这个rtp包之前有包丢失了。
			BufferedPacket* nextPacket = currentReorderBuffer->getNextCompletedPacket(packetLossPrecededThis);
			if (nextPacket == NULL)
				break;
			fNeedDelivery = False;
			if (nextPacket->useCount() == 0) {
				if (!processFECHeader(nextPacket, pCurrentPacketBeginsFrame, pCurrentPacketCompletesFrame)) {
					currentReorderBuffer->releaseUsedPacket(nextPacket);
					fNeedDelivery = True;
					continue;
				}
			}
			if (*pCurrentPacketBeginsFrame) {
				if (packetLossPrecededThis || *pCurrentPacketLossInFragmentedFrame) {  //当前这个rtp包是一帧的开始[开始新的一帧了]，若packetLossPrecededThis为true，表示先前有rtp包丢了，那么之前保存的数据全都不要了
					*fCurrentTo = fCurrentFECBuffer;  //让fCurrentTo指向buffer起始地址
					*fCurrentFrameSize = 0;
				}
				*pCurrentPacketLossInFragmentedFrame = False;
			}
			else if (packetLossPrecededThis) {
				*pCurrentPacketLossInFragmentedFrame = True;  //fCurrentPacketBeginsFrame 为false，则该rtp包是一帧数据的中间部分，但先前有包丢失了，所以拼不成一个完整帧了
			}
			if (*pCurrentPacketLossInFragmentedFrame) {
				currentReorderBuffer->releaseUsedPacket(nextPacket);
				fNeedDelivery = True;
				continue;
			}

			unsigned frameSize = 0;
			unsigned bytesTruncated = 0;
			unsigned short rtpSeqNo = 0;
			unsigned rtpTimestamp = 0;
			struct timeval presentationTime;
			Boolean hasBeenSyncedUsingRTCP = false;
			Boolean rtpMarkerBit = false;
			nextPacket->use(*fCurrentTo, *fCurrentMaxSize, frameSize, bytesTruncated,
				rtpSeqNo, rtpTimestamp,
				presentationTime, hasBeenSyncedUsingRTCP,
				rtpMarkerBit);
			*fCurrentFrameSize += frameSize;

			if (!nextPacket->hasUsableData()) {
				currentReorderBuffer->releaseUsedPacket(nextPacket);
			}
			else{
				DebugPrintf("FEC2DParityMultiplexor nextPacket has Usable Data,size: %u\n", nextPacket->dataSize());
			}


			if (*pCurrentPacketCompletesFrame && fFrameSize > 0) {
				if (bytesTruncated > 0) {
					//error! some data will dropped!
					DebugPrintf("FEC2DParityMultiplexor the total received frame size exceeds the client's buffer size:%d.%d bytes of trailing data will be dropped!\n", (int)MAX_FEC_BUFFER_SIZE, bytesTruncated);
				}
				pushFECRTPPacket(fCurrentFECBuffer, *fCurrentFrameSize);
				//DebugPrintf("succeed get %d, size:%u, seqenceNum:%u\n", bInterleave ? 116 : 115, *fCurrentFrameSize, (unsigned)((((u_int16_t)fCurrentFECBuffer[20]) << 8) | fCurrentFECBuffer[21]));
				resetFECBuffer(index);
			}
			else {
				*fCurrentTo += frameSize;
				*fCurrentMaxSize -= frameSize;
				fNeedDelivery = True;
			}
		}
	}
}

void FEC2DParityMultiplexor::pushFECRTPPacket(BufferedPacket* srcPacket)
{
	int payload = EXTRACT_BIT_RANGE(0, 7, srcPacket->data()[1]);
	if (payload == 115)
	{
		//DebugPrintf("receive: %d, size:%d, seq: %u, start :%d, end :%d\n", payload, srcPacket->dataSize(), (unsigned)((((u_int16_t)srcPacket->data()[20]) << 8) | srcPacket->data()[21]), srcPacket->data()[12 + 12] & 0x01, srcPacket->data()[12 + 12] & 0x02);
		preProcessFECPacket(NonInterleaveIndex, srcPacket);
	}
	else if (payload == 116)
	{
		//DebugPrintf("receive: %d, size:%d, seq: %u, start :%d, end :%d\n", payload, srcPacket->dataSize(), (unsigned)((((u_int16_t)srcPacket->data()[20]) << 8) | srcPacket->data()[21]), srcPacket->data()[12 + 12] & 0x01, srcPacket->data()[12 + 12] & 0x02);
		preProcessFECPacket(InterleaveIndex, srcPacket);
	}
	else if (96 == payload)
	{
		//DebugPrintf("receive: %d, size:%d, seq: %u\n", payload, srcPacket->dataSize(), (unsigned)((ntohl(*(u_int32_t*)(srcPacket->data()))) & 0xFFFF));
		pushFECRTPPacket(srcPacket->data(), srcPacket->dataSize());
	}
	else
	{
		DebugPrintf("receive unexpected type: %d, size:%d, seq: %u\n", payload, srcPacket->dataSize(), (unsigned)((ntohl(*(u_int32_t*)(srcPacket->data()))) & 0xFFFF));
		pushFECRTPPacket(srcPacket->data(), srcPacket->dataSize());
	}
}

void FEC2DParityMultiplexor::pushFECRTPPacket(unsigned char* buffer, unsigned bufferSize) {
	totalReceivedPacket++;
	RTPPacket* rtpPacket = RTPPacket::createNew(buffer, bufferSize);
	setHostSSRCIfNotSet(rtpPacket);  //获取原始rtp包的SSRC，赋值到hostSSRC

	if (first) 
		setBaseIfNotSet(rtpPacket);
	else
	{
		Boolean thereWasReadyRTPPackets = False;
		int payload = EXTRACT_BIT_RANGE(0, 7, rtpPacket->content()[1]);
		bool bRes = insertPacket(rtpPacket);  
		if (!bRes)
		{
			if (payload != 115 && payload != 116)
			{
				fRTPPackets.push(rtpPacket);
				thereWasReadyRTPPackets = True;
			}
			else
			{
				delete rtpPacket;  //是冗余包，它所在的Cluster已经超时被析构了，丢弃吧
			}
		}

		if (thereWasReadyRTPPackets)
			envir().taskScheduler().triggerEvent(fEventTriggerId, this);
	}
}

void FEC2DParityMultiplexor::sendNext(void* firstArg) {
	FEC2DParityMultiplexor* fec2DParityMultiplexor = (FEC2DParityMultiplexor*)firstArg;
	fec2DParityMultiplexor->repairPackets();
}

void FEC2DParityMultiplexor::repairPackets() {
    Boolean packetsAreAvailable = False;
    int clustersToErase = 0;
	
	for (int i = 0; i < superBuffer.size(); i++) {
		FECCluster* cluster = superBuffer.at(i);
		bool bGetAllRTPPacket = cluster->allRTPPacketsArePresent();
		bool bClusterTimeout = cluster->hasExpired(fRepairWindow);

		if (!bGetAllRTPPacket && !bClusterTimeout)
			break;  //进入break说明：该Cluster还未收齐数据包(非冗余包) 并且该Cluster未超时

		if (!bGetAllRTPPacket && bClusterTimeout) {
			//进入这里说明：该Cluster还未收齐数据包(非冗余包) 但 该Cluster已经超时了，尝试用冗余包把缺失的数据包恢复
			//FECDecoder::printCluster(cluster, fRow, fColumn);
			FECDecoder::repairCluster(cluster->rtpPackets(), fRow, fColumn, hostSSRC);   //what if ssrc is not set?
		}
		else{
			//进入这里说明：该Cluster收齐了所有的数据包(非冗余包)，无需repair
			DebugPrintf("Cluster %d, status: bGetAllRTPPacket is %d, bClusterTimeout is %d", cluster->base(), bGetAllRTPPacket, bClusterTimeout);
		}

		flushCluster(cluster->rtpPackets());  //将Cluster里的数据包(非冗余包)移到fRTPPackets，下面会通知上层来取
		clustersToErase++;
		packetsAreAvailable = True;
	}

    if (clustersToErase > 0) {
		for (int i = 0; i < clustersToErase; i++)
			delete superBuffer[i];
        superBuffer.erase( superBuffer.begin(), (superBuffer.begin() + clustersToErase)); 
    }

	if (packetsAreAvailable) 
		envir().taskScheduler().triggerEvent(fEventTriggerId, this);
	nextTask() = envir().taskScheduler().scheduleDelayedTask(20000, (TaskFunc*)sendNext, this);
}

void FEC2DParityMultiplexor::setHostSSRCIfNotSet(RTPPacket* rtpPacket) {
    if (ssrcWasSet) 
		return;
    int payload = EXTRACT_BIT_RANGE(0, 7, rtpPacket->content()[1]);
    if (payload == 115 || payload == 116) 
		return;
    hostSSRC = FECDecoder::extractSSRC(rtpPacket);
    ssrcWasSet = True;
}

void FEC2DParityMultiplexor::setBaseIfNotSet(RTPPacket* rtpPacket) {
    emergencyBuffer.push_back(rtpPacket);
    int didFind = False;
    u_int16_t newBase = 0;
    findBase(&didFind, &newBase);
    if (didFind == 0) 
		return;
    first = False;
    currentSequenceNumber = newBase;
    handleEmergencyBuffer(newBase);
}

bool FEC2DParityMultiplexor::insertPacket(RTPPacket* rtpPacket) 
{
	bool bRes = true;
    int payload = EXTRACT_BIT_RANGE(0, 7, rtpPacket->content()[1]);
	u_int16_t seq = (payload == 115 || payload == 116) ? FECDecoder::extractFECBase(rtpPacket) : FECDecoder::extractRTPSeq(rtpPacket);
	int sourcePacketCount = fRow * fColumn;
    u_int16_t newSeq = currentSequenceNumber + sourcePacketCount;
    if (newSeq >= currentSequenceNumber) 
	{
		if (seq >= newSeq)
		{
			u_int16_t diff = seq - currentSequenceNumber;
			if (diff > 30000)
			{
				DebugPrintf("receive %d, seq: %u,diff is too large, drop this packet\n", payload, seq);
				//Arbitrary number. This is a failsafe if a packet arrive out of in the 65535 rollover. Then the packet will be tossed.
				bRes = false;
			}
			else
			{
				updateCurrentSequenceNumber(seq, sourcePacketCount);
				//DebugPrintf("receive %d, seq: %u,now new a new cluster\n", payload, seq);
				FECCluster* fecCluster = FECCluster::createNew(currentSequenceNumber, fRow, fColumn);
				fecCluster->insertPacket(rtpPacket);
				superBuffer.push_back(fecCluster);
			}
		}
		else
		{
			FECCluster* fecCluster = findCluster(seq);
			if (fecCluster != NULL) 
			{
				//DebugPrintf("receive %d, seq: %u,find cluster, fbase:%u\n", payload, seq, fecCluster->base());
				fecCluster->insertPacket(rtpPacket);
			}
			else 
			{
				//DebugPrintf("receive %d, seq: %u, but not find cluster\n", payload, seq);
				bRes = false;
			}
		}
    }
    else
    {
		/*Special case*/
		DebugPrintf("into special case , u_int16_t overflow!");
		if (seq >= currentSequenceNumber || seq < newSeq)
		{
			//Find cluster
			FECCluster* fecCluster = findCluster(seq);
			if (fecCluster != NULL)
				fecCluster->insertPacket(rtpPacket);
			else
				bRes = false;
		}
		else
		{
			u_int16_t diff = seq - currentSequenceNumber;
			if (diff > 30000)
				bRes = false;
			else
			{
				updateCurrentSequenceNumber(seq, sourcePacketCount);
				FECCluster* fecCluster = FECCluster::createNew(currentSequenceNumber, fRow, fColumn);
				fecCluster->insertPacket(rtpPacket);
				superBuffer.push_back(fecCluster);
			}
		}
    }
	return bRes;
}

/*TODO:
	Fiks mod 65536, tror den er OK
	Memleaks
	Håndter flere clustere hvis base er laaangt fram i tid
*/
void FEC2DParityMultiplexor::handleEmergencyBuffer(u_int16_t base) {
	FECCluster* fecCluster = FECCluster::createNew(base, fRow, fColumn);
	Boolean thereWasReadyRTPPackets = False;
	superBuffer.push_back(fecCluster);

	for (int i = emergencyBuffer.size() - 1; i >= 0; i--) {
		RTPPacket* current = emergencyBuffer.at(i);

		int payload = EXTRACT_BIT_RANGE(0, 7, current->content()[1]);
		u_int16_t seq = (payload == 115 || payload == 116) ? FECDecoder::extractFECBase(current) : FECDecoder::extractRTPSeq(current);

		bool bInCluster = fecCluster->seqNumInCluster(seq);  //这个包属于这个组吗？
		if (bInCluster)
			fecCluster->insertPacket(current);
		else 
		{
			bool bRes = insertPacket(current);  //插入失败怎么办！
			if (!bRes)
			{
				if (payload != 115 && payload != 116)
				{
					fRTPPackets.push(current);
					thereWasReadyRTPPackets = True;
				}
				else
				{
					delete current;  //是冗余包，它所在的Cluster已经超时被析构了，丢弃吧
				}
			}
		}
		emergencyBuffer.pop_back();
	}
	if (thereWasReadyRTPPackets)
        envir().taskScheduler().triggerEvent(fEventTriggerId, this);
}


/*Utility methods*/
void FEC2DParityMultiplexor::updateCurrentSequenceNumber(u_int16_t newSeqnum, unsigned sourcePacketCount) {
    u_int16_t diff = newSeqnum - currentSequenceNumber;
    u_int16_t clustersBetween = diff / sourcePacketCount;
    currentSequenceNumber = currentSequenceNumber + clustersBetween * sourcePacketCount;


}

//mod 65536?
void FEC2DParityMultiplexor::findBase(int* didFind, u_int16_t* newBase) {
	for (int i = 0; i < emergencyBuffer.size(); i++) {
		RTPPacket* curr = emergencyBuffer.at(i);
		int payload = EXTRACT_BIT_RANGE(0, 7, curr->content()[1]);
		if (payload == 115 || payload == 116) {
			u_int16_t currBase = (((u_int16_t)curr->content()[20]) << 8) | curr->content()[21];

			for (int j = 0; j < emergencyBuffer.size(); j++) {
				RTPPacket* lol = emergencyBuffer.at(j);
				int payload2 = EXTRACT_BIT_RANGE(0, 7, lol->content()[1]);
				if (payload2 == (payload == 115 ? 116 : 115)) {
					u_int16_t lolBase = (((u_int16_t)lol->content()[20]) << 8) | lol->content()[21];
					if (currBase == lolBase) {
						*didFind = 1;
						*newBase = lolBase;
						return;
					}
				}
			}
		}
	}
}

FECCluster* FEC2DParityMultiplexor::findCluster(u_int16_t seqNum) {
	for (int i = 0; i < superBuffer.size(); i++) {
		if (superBuffer.at(i)->seqNumInCluster(seqNum)) {
			return superBuffer.at(i);
		}
	}
	return NULL;
}

void FEC2DParityMultiplexor::flushCluster(RTPPacket** cluster) {
	int size = fRow * fColumn;
	for (int i = 0; i < size; i++) {
		int index = i + i / fColumn;
		RTPPacket* rtpPacket = cluster[index];
		if (rtpPacket != NULL) {
            fRTPPackets.push(rtpPacket);
        }
		cluster[index] = NULL;
	}
}

void FEC2DParityMultiplexor::printSuperBuffer() {
    if (superBuffer.empty()) {
		DebugPrintf("super buffer is empty!\n");
        return;
    }

	DebugPrintf("Start of superbuffer:\n");
    for (int i = 0; i < superBuffer.size(); i++) {
        FECCluster* cluster = superBuffer.at(i);
        FECDecoder::printCluster(cluster, fRow, fColumn);
    }
	DebugPrintf("End of superbuffer:\n");

}
