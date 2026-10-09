// Silent JAudio playback engine (P2_AUDIO_ENABLED=0).
//
// Gameplay objects own real JAudio sound objects (JAIObject, JAIAnimation and
// the PSM classes are compiled from the decomp). Only the engine below them is
// replaced: JAIBasic keeps its original state and camera table, and every
// request to start a sound completes as "not started", leaving the caller's
// handle null exactly as when the console has no free voice.
#include "types.h"
#include "JSystem/JAudio/JAI/JAIBasic.h"
#include "JSystem/JAudio/JAI/JAInter.h"
#include "Dolphin/mtx.h"
#include "JSystem/JAudio/JAI/JAIConst.h"
#include "JSystem/JAudio/JAI/JAISequence.h"
#include "JSystem/JAudio/JAI/JAIStream.h"
#include "JSystem/JAudio/JAI/JAInter/BankWave.h"
#include "JSystem/JAudio/JAI/JAInter/InitData.h"
#include "JSystem/JAudio/JAI/JAInter/SeMgr.h"
#include "JSystem/JAudio/JAI/JAInter/StreamMgr.h"
#include "JSystem/JAudio/JAS/JASAramStream.h"
#include "JSystem/JAudio/JAS/JASBank.h"
#include "JSystem/JAudio/JAS/JASDriver.h"
#include "JSystem/JAudio/JAS/JASDvd.h"
#include "JSystem/JAudio/JAS/JASHeap.h"
#include "JSystem/JAudio/JAS/JASResArcLoader.h"
#include "JSystem/JAudio/JAS/JASTrack.h"
#include "JSystem/JAudio/JAS/JASWave.h"
#include "JSystem/JAudio/JAI/JAIGlobalParameter.h"
#include "JSystem/JKernel/JKRArchive.h"
#include <cstring>

JAIBasic* JAIBasic::msBasic;
JKRHeap* JAIBasic::msCurrentHeap;
bool JAIBasic::msStopMode;
u32 JAIBasic::msAudioStopTime;
f32 JAIBasic::msDspLevel;
f32 JAIBasic::msAutoLevel;
f32 JAIBasic::msAutoDif;
f32 JAIBasic::msDspDif;
u8 JAIBasic::msStopStatus = 3;

namespace {
// Listener cameras until the game supplies its own through setCameraInfo:
// origin position, identity view, so camera-relative sound math is defined.
constexpr u32 kCameraCount = 4;
JAInter::Camera* silentCameras()
{
	static Mtx identity[kCameraCount];
	static Vec origin[kCameraCount];
	static JAInter::Camera cameras[kCameraCount];
	static bool initialized = false;
	if (!initialized) {
		for (u32 i = 0; i < kCameraCount; ++i) {
			PSMTXIdentity(identity[i]);
			cameras[i] = JAInter::Camera(&origin[i], &origin[i], &identity[i]);
		}
		initialized = true;
	}
	return cameras;
}
// msBasic before the game constructs its own engine (PSSystem::SysIF).
JAIBasic sBootEngine;
} // namespace

// Original constructor state (JAIBasic.cpp) without the JAS DRAM heap.
JAIBasic::JAIBasic()
{
	msBasic       = this;
	mFlags._00    = false;
	mFlags._01    = false;
	mFlags._02    = false;
	mFlags._03    = false;
	mFlags._04    = false;
	_14           = 0;
	mCameras      = silentCameras();
	mCurrentTick  = 0;
	mFileLoadType = 2;
	_1C           = nullptr;
	mHeap         = nullptr;
	mRawDataPtr   = nullptr;
	msCurrentHeap = nullptr;
}

JAISequence* JAIBasic::makeSequence() { return nullptr; }
JAISe* JAIBasic::makeSe() { return nullptr; }
JAIStream* JAIBasic::makeStream() { return nullptr; }
u16 JAIBasic::getMapInfoFxline(u32) { return 0; }
BOOL JAIBasic::getMapInfoGround(u32) { return FALSE; }
f32 JAIBasic::getMapInfoFxParameter(u32) { return 0.0f; }
void JAIBasic::setSeExtParameter(JAISound*) { }
void JAIBasic::setRegisterTrackCallback() { }

void JAIBasic::setCameraInfo(Vec* position, Vec* target, f32 (*view)[4], u32 index)
{
	if (index < kCameraCount) {
		mCameras[index] = JAInter::Camera(position, target, reinterpret_cast<Mtx*>(view));
	}
}

// Sequences and streams come from pools that console code treats as always
// available, so a start yields an idle object tagged with the request's ID.
// Sound effects compete for voices and may fail, so they are never started.
// Like JAISound::initParameter, the sound remembers its owner's handle so a
// release can clear it (owners test the handle to see if audio is playing).
template <typename T>
static void startIdle(JAIBasic* engine, T** handle, u32 id, T* (JAIBasic::*make)(), JAInter::SoundInfo* info)
{
	if (handle && !*handle) {
		*handle = (engine->*make)();
		if (*handle) {
			(*handle)->mSoundID           = id;
			(*handle)->mSoundInfo         = info;
			(*handle)->mMainSoundPPointer = reinterpret_cast<void**>(handle);
		}
	}
}
// Releases finish immediately (no fade is ever in progress): the sound goes
// inactive and its owner's handle is cleared, as the real managers do.
static void releaseIdle(JAISound* sound)
{
	if (sound) {
		sound->mState = SOUNDSTATE_Inactive;
		sound->clearMainSoundPPointer();
	}
}
void JAIBasic::startSoundBasic(u32 id, JAISound** handle, JAInter::Actor*, u32, u8, JAInter::SoundInfo* info)
{
	switch (id & JAISoundID_TypeMask) {
	case JAISoundID_Type_Sequence:
		startIdle(this, reinterpret_cast<JAISequence**>(handle), id, &JAIBasic::makeSequence, info);
		break;
	case JAISoundID_Type_Stream:
		startIdle(this, reinterpret_cast<JAIStream**>(handle), id, &JAIBasic::makeStream, info);
		break;
	default:
		break;
	}
}
void JAIBasic::startSoundBasic(u32 id, JAISequence** handle, JAInter::Actor*, u32, u8, JAInter::SoundInfo* info)
{
	startIdle(this, handle, id, &JAIBasic::makeSequence, info);
}
void JAIBasic::startSoundBasic(u32, JAISe**, JAInter::Actor*, u32, u8, JAInter::SoundInfo*) { }
void JAIBasic::startSoundBasic(u32 id, JAIStream** handle, JAInter::Actor*, u32, u8, JAInter::SoundInfo* info)
{
	startIdle(this, handle, id, &JAIBasic::makeStream, info);
}

// Nothing plays, so there is no frame work and nothing to stop.
void JAIBasic::processFrameWork() { }
void JAIBasic::stopSoundHandle(JAISound* sound, u32) { releaseIdle(sound); }
void JAIBasic::stopAllSe(u8) { }

// Engine initialisation. The driver thread, DSP, heaps and AAF parsing are
// skipped: nothing is loaded, so no sound table entry, bank or wave exists.
void JAIBasic::initDriver(JKRSolidHeap*, u32, u8) { }
// The data part of initInterfaceMain stays: BGM objects read sequence file
// sizes from the sequence archive, so mount it as initArchive does.
void JAIBasic::initInterface(u8)
{
	if (!JAInter::SequenceMgr::getArchivePointer()) {
		char archiveName[100];
		JAInter::SequenceMgr::getArchiveName(archiveName);
		JAInter::SequenceMgr::setArchivePointer(
		    JKRMountArchive(archiveName, JKRArchive::EMM_Dvd, getCurrentJAIHeap(), JKRArchive::EMD_Head));
	}
}
void JAIBasic::initHeap() { }
void JAIBasic::setInitFileLoadSwitch(u8 type) { mFileLoadType = type; }
u16 JAIBasic::setParameterSeqSync(JASTrack*, u16) { return 0; }

JKRSolidHeap* JASDram;
JMath::TRandom_fast_ JAInter::Const::random(0);

// Sound-effect, sequence and stream managers: no buffers are ever allocated.
namespace JAInter {
namespace SeMgr {
JAISequence* seHandle;
static f32 sCategoryVolume[256];
f32* seCategoryVolume = sCategoryVolume;
void releaseSeBuffer(JAISe* se, u32) { releaseIdle(reinterpret_cast<JAISound*>(se)); }
void setSeSequenceStartCallback(StartCallback) { }
} // namespace SeMgr
namespace SequenceMgr {
JKRArchive* arcPointer;
JKRArchive* getArchivePointer() { return arcPointer; }
void setArchivePointer(JKRArchive* archive) { arcPointer = archive; }
void getArchiveName(char* path)
{
	path[0] = '\0';
	if (JAIGlobalParameter::getParamAudioResPath() != nullptr) {
		strcat(path, JAIGlobalParameter::getParamAudioResPath());
	}
	strcat(path, JAIGlobalParameter::getParamSequenceArchivesPath());
	strcat(path, JAIGlobalParameter::getParamSequenceArchivesFileName());
}
// Sequences come from a pool on console, so starting one always yields an
// object; hand out an idle one from the engine's factory (SysIF::makeSequence).
void storeSeqBuffer(JAISequence** handle, Actor*, u32, u32, u8, SoundInfo*)
{
	if (handle && !*handle && JAIBasic::msBasic) {
		*handle = JAIBasic::msBasic->makeSequence();
	}
}
void releaseSeqBuffer(JAISequence* seq, u32) { releaseIdle(seq); }
SeqUpdateData* getPlayTrackInfo(u32) { return nullptr; }
void setCustomHeapCallback(CustomHeapCallback) { }
} // namespace SequenceMgr
namespace StreamMgr {
StreamUpdateData* streamUpdate;
void releaseStreamBuffer(JAIStream* stream, u32) { releaseIdle(stream); }
JASAramStream* getStreamObjectPointer() { return nullptr; }
u32 getSystemStatus() { return 0; }
} // namespace StreamMgr
namespace BankWave {
void setInitCallback(InitCallback) { }
void setFirstLoadCallback(LoadCallback) { }
void setSecondLoadCallback(LoadCallback) { }
} // namespace BankWave
namespace InitData {
u32* aafPointer;
void setBnkInitCallback(void (*)(u32*)) { }
void setWsInitCallback(void (*)(u32*)) { }
} // namespace InitData
namespace SystemInterface {
void outerInit(SeqUpdateData*, JASTrack*, u32, u16, u8) { }
} // namespace SystemInterface
u32 routeToTrack(u32) { return 0; }
} // namespace JAInter

// Resource loaders complete immediately: there is nothing to load, and
// asynchronous callers are told the request finished so no wait stalls.
namespace JASBankMgr {
void init(int) { }
bool registBankBNK(int, void*) { return true; }
bool assignWaveBank(int, int) { return true; }
} // namespace JASBankMgr
namespace JASWaveBankMgr {
void init(int) { }
bool registWaveBankWS(int, void*) { return true; }
bool loadWave(int, int, JASHeap*) { return true; }
bool loadWaveTail(int, int, JASHeap*) { return true; }
bool eraseWave(int, int) { return true; }
} // namespace JASWaveBankMgr
namespace JASWaveArcLoader {
void init(JASHeap*) { }
void setCurrentDir(const char*) { }
} // namespace JASWaveArcLoader
namespace JASResArcLoader {
// Sequence data is real game data (BGM objects parse it), so it is read for
// real; only the audio DVD thread is replaced by a synchronous read.
size_t getResSize(JKRArchive* archive, u16 resourceID)
{
	JKRArchive::SDIFileEntry* file = archive ? archive->findIdResource(resourceID) : nullptr;
	return file ? file->getSize() : 0;
}
int loadResourceAsync(JKRArchive* archive, u16 id, u8* buffer, u32 size, LoadCallback callback, uintptr_t cbArg)
{
	const u32 readResult = archive ? archive->readResource(buffer, size, id) : 0;
	if (callback) {
		callback(readResult, cbArg);
	}
	return 1; // The command was queued (and here, already completed).
}
} // namespace JASResArcLoader
namespace JASDvd {
void checkPassDvdT(uintptr_t arg, u32*, JASDvdCallback callback)
{
	if (callback) {
		callback(arg);
	}
}
} // namespace JASDvd
namespace JASDriver {
void setLevel(f32, f32, f32) { }
} // namespace JASDriver
bool JASAramStream::pause(bool) { return false; }

// Tracks carry no sequence: registers and ports read as zero.
JASVibrate::JASVibrate() { std::memset(static_cast<void*>(this), 0, sizeof(*this)); }
JASTrack::JASTrack()
{
	auto* bytes = reinterpret_cast<unsigned char*>(this) + sizeof(JSUList<JASChannel>);
	std::memset(bytes, 0, sizeof(JASTrack) - sizeof(JSUList<JASChannel>));
}
void JASTrack::setParam(int, f32, int) { }
u16 JASTrack::readReg16(u8) { return 0; }
bool JASTrack::writePortAppDirect(u32, u16) { return false; }
bool JASTrack::readPortAppDirect(u32, u16* value)
{
	if (value) {
		*value = 0;
	}
	return false;
}
bool JASTrack::writePortApp(u32, u16) { return false; }
bool JASTrack::readPortApp(u32, u16* value)
{
	if (value) {
		*value = 0;
	}
	return false;
}
void JASTrack::pause(bool, bool) { }
void JASTrack::setTempo(u16) { }
void JASTrack::setNoteMask(u8) { }
void JASTrack::muteTrack(bool) { }
void JASTrack::registerSeqCallback(SeqCallback) { }
