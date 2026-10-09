"""JAudio (JAS/JAI/JAD/JAL/JAU) 64-bit and byte-order patches for real audio.

Applied to the prepared tree by prepare_source.py. The silent sound system
(src/audio_silent.cpp) remains the default build; these files compile in the
p2_jaudio_objects target until the real driver links (P2_AUDIO).
"""
import re


def apply(destination, replace, sub_exact, native_body):
    # Clang rejects redeclaring a namespace-scope extern as static; the header
    # declarations are the definitions' declarations.
    for path, names in (("src/JSystem/JAudio/JAS/JASDSPInterface.cpp", ("CH_BUF", "FX_BUF", "sDSPVolume")),
                        ("src/JSystem/JAudio/JAS/JASDriverIF.cpp", ("MAX_MIXERLEVEL", "MAX_AUTOMIXERLEVEL", "JAS_SYSTEM_OUTPUT_MODE")),
                        ("src/JSystem/JAudio/JAI/JAIStreamMgr.cpp", ("allocCallback", "deallocCallback", "externalAramCallback"))):
        text = (destination / path).read_text()
        for name in names:
            text = sub_exact(r"(?m)^static\s+(?=[^;\n]*\b" + name + r"\b)", "", text, 1, path + ":" + name)
        (destination / path).write_text(text)
    replace("src/JSystem/JAudio/JAS/JASBNKParser.cpp", "static size_t JASBNKParser::sUsedHeapSize = 0;",
            "size_t JASBNKParser::sUsedHeapSize = 0;")

    # Callback arguments carry host pointers (PSSystem passes objects through
    # them); the silent layer already widened the typedefs to uintptr_t.
    replace("src/JSystem/JAudio/JAS/JASDvdThread.cpp", "void JASDvd::checkPassDvdT(u32 p1, u32* p2, JASDvdCallback p3)",
            "void JASDvd::checkPassDvdT(uintptr_t p1, u32* p2, JASDvdCallback p3)")
    replace("src/JSystem/JAudio/JAS/JASResArcLoader.cpp", "LoadCallback callback, u32 cbArg)", "LoadCallback callback, uintptr_t cbArg)")
    replace("src/JSystem/JAudio/JAS/JASResArcLoader.cpp", "return ((int)receiveBuffer != RESARCMSG_Success) ? 0 : size;",
            "return ((int)(intptr_t)receiveBuffer != RESARCMSG_Success) ? 0 : size;")
    replace("include/JSystem/JAudio/JAI/JAInter/BankWave.h", "void finishSceneSet(u32);", "void finishSceneSet(uintptr_t);")
    replace("src/JSystem/JAudio/JAI/JAIBankWave.cpp", "void JAInter::BankWave::finishSceneSet(u32 flag)",
            "void JAInter::BankWave::finishSceneSet(uintptr_t flag)")
    replace("include/JSystem/JAudio/JAI/JAInter.h", "void checkDvdLoadArc(u32, u32);\nvoid checkCustomDvdLoadArc(u32, u32);",
            "void checkDvdLoadArc(u32, uintptr_t);\nvoid checkCustomDvdLoadArc(u32, uintptr_t);")
    replace("src/JSystem/JAudio/JAI/JAISequenceMgr.cpp", "void checkDvdLoadArc(u32 p1, u32 p2)", "void checkDvdLoadArc(u32 p1, uintptr_t p2)")
    replace("src/JSystem/JAudio/JAI/JAISequenceMgr.cpp", "void checkCustomDvdLoadArc(u32 p1, u32 index)", "void checkCustomDvdLoadArc(u32 p1, uintptr_t index)")
    replace("src/JSystem/JAudio/JAI/JAISequenceMgr.cpp", "if ((u32)dataPtr == 0xFFFFFFFF) {", "if ((u32)(uintptr_t)dataPtr == 0xFFFFFFFF) {")
    # The constructor is defined inside namespace SequenceMgr, which Clang rejects.
    replace("src/JSystem/JAudio/JAI/JAISequenceMgr.cpp", "JAInter::SeqUpdateData::SeqUpdateData()",
            "} // namespace SequenceMgr\nSeqUpdateData::SeqUpdateData()")
    replace("src/JSystem/JAudio/JAI/JAISequenceMgr.cpp",
            "_44           = new (JAIBasic::getCurrentJAIHeap(), 0x20) u32[JAIGlobalParameter::getParamSeqTrackMax() + 1];\n}",
            "_44           = new (JAIBasic::getCurrentJAIHeap(), 0x20) u32[JAIGlobalParameter::getParamSeqTrackMax() + 1];\n}\nnamespace SequenceMgr {")

    # Stay-heap arithmetic on host addresses.
    replace("src/JSystem/JAudio/JAI/JAISequenceHeap.cpp", "if (p1 + u32(stayPtr) < u32(basePtr) + JAIGlobalParameter::getParamStayHeapSize()",
            "if (p1 + uintptr_t(stayPtr) < uintptr_t(basePtr) + JAIGlobalParameter::getParamStayHeapSize()")
    replace("src/JSystem/JAudio/JAI/JAISequenceHeap.cpp", "(void*)(u32(getStayHeap(sStayHeapCount)->getPointer()) + ALIGN_PREV(p1, 32));",
            "(void*)(uintptr_t(getStayHeap(sStayHeapCount)->getPointer()) + ALIGN_PREV(p1, 32));")
    replace("src/JSystem/JAudio/JAI/JAISequenceHeap.cpp", "basePtr = (void*)(u32(basePtr) + 32);", "basePtr = (void*)(uintptr_t(basePtr) + 32);")
    # Stream and ARAM-heap "pointers" hold ARAM addresses, which fit in 32 bits.
    replace("src/JSystem/JAudio/JAI/JAIStreamMgr.cpp", "start           = (u32)info.mStart;", "start           = (u32)(uintptr_t)info.mStart;")
    replace("src/JSystem/JAudio/JAI/JAIStreamMgr.cpp", "start  = (u32)aramBufferHeap->mBase;", "start  = (u32)(uintptr_t)aramBufferHeap->mBase;")
    replace("src/JSystem/JAudio/JAI/JAIStreamMgr.cpp", "streamSystem->init(reinterpret_cast<u32>(info.mStart),",
            "streamSystem->init((u32)reinterpret_cast<uintptr_t>(info.mStart),")
    replace("src/JSystem/JAudio/JAS/JASAramStream.cpp", "dspChan->mSampleOffset - (u32)chan->mWaveData;", "dspChan->mSampleOffset - (u32)(uintptr_t)chan->mWaveData;")
    replace("src/JSystem/JAudio/JAS/JASAramStream.cpp", "switch ((u32)msg) {", "switch ((u32)(uintptr_t)msg) {")
    replace("src/JSystem/JAudio/JAS/JASAramStream.cpp", "switch ((u32)msg & 0xFF) {", "switch ((u32)(uintptr_t)msg & 0xFF) {")
    replace("src/JSystem/JAudio/JAS/JASAramStream.cpp", "channelStop((u32)msg >> 16);", "channelStop((u32)(uintptr_t)msg >> 16);")
    replace("src/JSystem/JAudio/JAS/JASChannel.cpp", "dspChan->setWaveInfo(*mWaveInfo, (u32)mWaveData, _C8);",
            "dspChan->setWaveInfo(*mWaveInfo, (u32)(uintptr_t)mWaveData, _C8);")
    replace("src/JSystem/JAudio/JAS/JASChannel.cpp", "dspChan->setOscInfo((u32)mWaveData);", "dspChan->setOscInfo((u32)(uintptr_t)mWaveData);")
    replace("src/JSystem/JAudio/JAS/JASHeapCtrl.cpp", "u32 offset = u32(it->mBase) - u32(minBase);", "u32 offset = u32(uintptr_t(it->mBase) - uintptr_t(minBase));")
    replace("src/JSystem/JAudio/JAS/JASHeapCtrl.cpp", "audioAramHeap.mSize      = aramSize - ((u32)audioAramHeap.mBase - aramBase);",
            "audioAramHeap.mSize      = aramSize - ((u32)(uintptr_t)audioAramHeap.mBase - aramBase);")
    replace("src/JSystem/JAudio/JAS/JASHeapCtrl.cpp", "((u32*)mems)[0] = (u32)mNextFreeBlock;", "((void**)mems)[0] = mNextFreeBlock; // free-list link, pointer sized")
    # Loud diagnostic: a sequence jump/call far outside any sequence file.
    seqp = "src/JSystem/JAudio/JAS/JASSeqParser.cpp"
    replace(seqp, "\t\t\tctrl->jump(data);\n",
            "\t\t\tif (data > 0x100000) std::fprintf(stderr, \"*** seq jmp flag %02x data %08x reg4 %04x reg5 %04x\\n\", flag, data, track->readReg16(4), track->readReg16(5));\n\t\t\tctrl->jump(data);\n")
    replace(seqp, "\t\tctrl->mCurrentFilePtr                         = ctrl->mRawFilePtr + data;",
            "\t\tif (data > 0x100000) std::fprintf(stderr, \"*** seq call flag %02x data %08x\\n\", flag, data);\n\t\tctrl->mCurrentFilePtr                         = ctrl->mRawFilePtr + data;")
    file = destination / seqp
    file.write_text("#include <cstdio>\n" + file.read_text())
    seqc = "src/JSystem/JAudio/JAS/JASSeqCtrl.cpp"
    replace(seqc, "\tmCurrentFilePtr = mRawFilePtr + offset;",
            "\tif (offset > 0x100000) { std::fprintf(stderr, \"*** seq start offset %08x\\n\", offset); void* f[8]; backtrace_symbols_fd(f, backtrace(f, 8), 2); }\n\tmCurrentFilePtr = mRawFilePtr + offset;")
    file = destination / seqc
    file.write_text("#include <cstdio>\n#include <execinfo.h>\n" + file.read_text())
    # Envelope tables a sequence selects (oscSetupSimpleEnv) point into the
    # big-endian sequence file; bank tables are converted at load. Convert these
    # too, into a fixed cache (audio thread: no allocation), keyed by source.
    track = "src/JSystem/JAudio/JAS/JASTrack.cpp"
    replace(track, "void JASTrack::oscSetupSimpleEnv(u8 setupType, u32 offset)\n{",
            """namespace {
struct P2NativeEnvelope { const u8* source; s16 table[3 * 32]; };
P2NativeEnvelope sNativeEnvelopes[256];
unsigned sNextNativeEnvelope;
const s16* p2NativeEnvelope(const u8* source)
{
	for (auto& entry : sNativeEnvelopes)
		if (entry.source == source) return entry.table;
	P2NativeEnvelope& entry = sNativeEnvelopes[sNextNativeEnvelope++ & 255];
	entry.source = source;
	for (int i = 0; i < 3 * 32; i += 3) {
		for (int k = 0; k < 3; ++k) entry.table[i + k] = p2_read_big<s16>(source + 2 * (i + k));
		if (entry.table[i] >= JASOscillator::ENVMODE_Loop) return entry.table; // Loop, Hold, Stop end a table
	}
	entry.table[3 * 31] = JASOscillator::ENVMODE_Stop; // never seen on disc: end it safely
	return entry.table;
}
} // namespace

void JASTrack::oscSetupSimpleEnv(u8 setupType, u32 offset)
{""")
    replace(track, "\t\tmOscData[0].mAttack = reinterpret_cast<s16*>(mSeqCtrl.mRawFilePtr + offset);",
            "\t\tmOscData[0].mAttack = const_cast<s16*>(p2NativeEnvelope(mSeqCtrl.mRawFilePtr + offset));")
    replace(track, "\t\tmOscData[0].mRelease = reinterpret_cast<const s16*>(mSeqCtrl.mRawFilePtr + offset);",
            "\t\tmOscData[0].mRelease = p2NativeEnvelope(mSeqCtrl.mRawFilePtr + offset);")
    # Loud diagnostics: a pool running dry, or two threads inside one
    # "SingleThreaded" pool at once (the console serialised them by interrupt).
    replace("src/JSystem/JAudio/JAS/JASHeapCtrl.cpp", "void* JASGenericMemPool::alloc(u32)\n{\n\tvoid** mem = mNextFreeBlock;\n\tif (mem == nullptr) {\n\t\treturn nullptr;\n\t}",
            "void* JASGenericMemPool::alloc(u32 size)\n{\n\tP2PoolGuard guard(this, \"alloc\");\n\tvoid** mem = mNextFreeBlock;\n\tif (mem == nullptr) {\n"
            "\t\tstatic int reports;\n\t\tif (reports++ < 8) std::fprintf(stderr, \"*** JAS pool %p empty: alloc of 0x%x returns null (free count %d)\\n\", this, size, mFreeMemCount);\n"
            "\t\treturn nullptr;\n\t}\n"
            "\tstatic int lowest = 1 << 30;\n\tif (mFreeMemCount - 1 < lowest && mFreeMemCount - 1 < 24) { lowest = mFreeMemCount - 1;\n"
            "\t\tstd::fprintf(stderr, \"[JASPOOL] tick %u pool %p new low: %d free\\n\", p2_tick_now, this, lowest); }")
    replace("src/JSystem/JAudio/JAS/JASHeapCtrl.cpp", "void JASGenericMemPool::free(void* mem, u32)\n{\n",
            "void JASGenericMemPool::free(void* mem, u32)\n{\n\tP2PoolGuard guard(this, \"free\");\n")
    replace("src/JSystem/JAudio/JAS/JASHeapCtrl.cpp", "void JASGenericMemPool::newMemPool(u32 size, int memCount)\n{",
            """namespace {
std::atomic<uintptr_t> sPoolOwners[64];
struct P2PoolGuard {
	std::atomic<uintptr_t>& slot;
	P2PoolGuard(const void* pool, const char* what) : slot(sPoolOwners[(uintptr_t(pool) >> 4) & 63]) {
		const uintptr_t self = uintptr_t(pthread_self());
		const uintptr_t other = slot.exchange(self);
		static int reports;
		if (other && other != self && reports++ < 8)
			std::fprintf(stderr, "*** JAS pool %p: %s while another thread is inside (tick %u)\\n", pool, what, p2_tick_now);
	}
	~P2PoolGuard() { slot.store(0); }
};
} // namespace

void JASGenericMemPool::newMemPool(u32 size, int memCount)
{""")
    file = destination / "src/JSystem/JAudio/JAS/JASHeapCtrl.cpp"
    file.write_text("#include <atomic>\n#include <cstdio>\n#include <pthread.h>\nextern \"C\" uint32_t p2_tick_now;\n" + file.read_text())
    replace("src/JSystem/JAudio/JAS/JASWaveArcLoader.cpp", "(u32)castedArgs->_08, Switch_0", "(u32)(uintptr_t)castedArgs->_08, Switch_0")
    replace("src/JSystem/JAudio/JAS/JASAudioThread.cpp", "switch ((int)msg) {", "switch ((int)(intptr_t)msg) {")
    replace("src/JSystem/JAudio/JAS/JASBankMgr.cpp", "setVir2PhyTable(*(u32*)((int)data + 8), bankIndex); // pointer jank",
            "setVir2PhyTable(p2_read_big<u32>((u8*)data + 8), bankIndex);")
    replace("src/JSystem/JAudio/JAS/JASBankMgr.cpp", "WEAKFUNC\nJASMemPool<JASChannel, JASThreadingModel::SingleThreaded>*\n",
            "template <>\nJASMemPool<JASChannel, JASThreadingModel::SingleThreaded>*\n")
    replace("src/JSystem/JAudio/JAS/JASBankMgr.cpp",
            "JASSingletonHolder<JASMemPool<JASChannel, JASThreadingModel::SingleThreaded>, JASCreationPolicy::NewFromRootHeap>::sInstance;",
            "JASSingletonHolder<JASMemPool<JASChannel, JASThreadingModel::SingleThreaded>, JASCreationPolicy::NewFromRootHeap>::sInstance = nullptr;")
    replace("src/JSystem/JAudio/JAS/JASSeqParser.cpp", "track->mExtBuffer->setFirFilter((s16*)((u32)track->getSeq()->getAddr(args[0])));",
            "track->mExtBuffer->setFirFilter((s16*)track->getSeq()->getAddr(args[0]));")
    # Debug printf argument registers; only computed, never printed.
    replace("src/JSystem/JAudio/JAS/JASSeqParser.cpp", "int registers[4];", "intptr_t registers[4];")
    replace("src/JSystem/JAudio/JAS/JASSeqParser.cpp", "registers[i] = (int)&track->getSeq()->mRawFilePtr[registers[i]];",
            "registers[i] = (intptr_t)&track->getSeq()->mRawFilePtr[registers[i]];")
    replace("src/JSystem/JAudio/JAS/JASTrack.cpp", "\t\tif (_357 & 0x2) {\n\t\t\treturn;\n\t\t}", "\t\tif (_357 & 0x2) {\n\t\t\treturn 0;\n\t\t}")
    for table in ("0xC000", "0x8001"):
        replace("src/JSystem/JAudio/JAS/JASPlayer_impl.cpp", ", " + table + ",", ", (s16)" + table + ",")
    # The host mixer reads channel state directly; there is no DSP table to set up.
    replace("src/JSystem/JAudio/JAS/JASDSPInterface.cpp", "DsetupTable(0x40, (u32)CH_BUF, (u32)DSPRES_FILTER, (u32)DSPADPCM_FILTER, (u32)FX_BUF);",
            "// DsetupTable: no DSP; the host mixer reads CH_BUF and FX_BUF directly.")
    # Copies with the same contract as the original word loops.
    native_body("src/JSystem/JAudio/JAS/JASCalc.cpp", "imixcopy",
                "\tfor (; n != 0; n--) {\n\t\t*dst++ = *s1++;\n\t\t*dst++ = *s2++;\n\t}")
    native_body("src/JSystem/JAudio/JAS/JASCalc.cpp", "bcopyfast", "\tmemcpy(dest, src, size & ~u32(15));")
    native_body("src/JSystem/JAudio/JAS/JASCalc.cpp", "bcopy", "\tmemcpy(dest, src, size);")
    native_body("src/JSystem/JAudio/JAS/JASCalc.cpp", "bzerofast", "\tmemset(dest, 0, size);")
    native_body("src/JSystem/JAudio/JAS/JASCalc.cpp", "bzero", "\tmemset(dest, 0, size);")
    replace("src/JSystem/JAudio/JAS/JASCalc.cpp", '#include "Dolphin/os.h"', '#include "Dolphin/os.h"\n#include <string.h>')
    # AI DMA is replaced by host output; addresses here are only passed through.
    replace("src/JSystem/JAudio/JAS/JASAiCtrl.cpp", "AIInitDMA((u32)sDmaDacBuffer[2], size);", "AIInitDMA((u32)(uintptr_t)sDmaDacBuffer[2], size);")
    replace("src/JSystem/JAudio/JAS/JASAiCtrl.cpp", "AIInitDMA((u32)rsp, getDacSize() * 2);", "AIInitDMA((u32)(uintptr_t)rsp, getDacSize() * 2);")
    replace("src/JSystem/JAudio/JAS/JASAiCtrl.cpp",
            "JASDsp::syncFrame(getSubFrames(), (u32)sDspDacBuffer[sDspDacWriteBuffer], (u32)(sDspDacBuffer[sDspDacWriteBuffer] + frameSamples));",
            "JASDsp::syncFrame(getSubFrames(), (u32)(uintptr_t)sDspDacBuffer[sDspDacWriteBuffer], (u32)(uintptr_t)(sDspDacBuffer[sDspDacWriteBuffer] + frameSamples));")
    replace("src/JSystem/JAudio/JAS/JASBankMgr.cpp", '#include "JSystem/JAudio/JAS/JASBank.h"',
            '#include "JSystem/JAudio/JAS/JASBank.h"\n#include "p2_endian.h"')

    # AAF (PSound.aaf) init table. Words are big-endian; the console relocated
    # 32-bit offsets into pointer slots in place, so the host builds native
    # tables. Sections used by Pikmin 2: 1 (sound table), 2 (banks), 3 (wave
    # systems), 5 (streams), 6 (scenes), 7 (FX scenes).
    replace("src/JSystem/JAudio/JAI/JAIInitData.cpp", '#include "JSystem/JAudio/JAI/JAInter/BankWave.h"',
            '#include "JSystem/JAudio/JAI/JAInter/BankWave.h"\n#include "JSystem/JAudio/JAI/JAInter/InitData.h"\n#include "p2_endian.h"\n'
            'static u32 aafWord(u32 index) { return p2_read_big<u32>(JAInter::InitData::aafPointer + index); }')
    native_body("src/JSystem/JAudio/JAI/JAIInitData.cpp", "JAInter::InitData::checkInitDataOnMemory", """\tu32 at = 0;
	for (bool more = true; more;) {
		switch (aafWord(at++)) {
		case 0:
			more = false;
			break;
		case 1: {
			const u32 size = aafWord(at + 1);
			SoundTable::init(transInitDataFile((u8*)aafPointer + aafWord(at), size), size);
			at += 3;
			break;
		}
		case 2:
			bnkInitCallback(&at);
			break;
		case 3:
			wsInitCallback(&at);
			break;
		case 4:
			at += 3;
			break;
		case 5:
			// An 8-byte block whose first slot holds the stream table pointer.
			StreamMgr::initOnCodeStrm        = transInitDataFile((u8*)(aafPointer + at), 8);
			*(u8**)StreamMgr::initOnCodeStrm = transInitDataFile((u8*)aafPointer + aafWord(at), aafWord(at + 1));
			StreamMgr::streamList            = *(u16**)StreamMgr::initOnCodeStrm;
			at += 3;
			break;
		case 6: {
			u32* table = (u32*)transInitDataFile((u8*)aafPointer + aafWord(at), aafWord(at + 1));
			const u32 scenes = p2_read_big<u32>(table);
			JAIGlobalParameter::setParamSoundSceneMax(scenes);
			u8** pointers = new (JAIBasic::msCurrentHeap, 0x20) u8*[scenes];
			for (u32 i = 0; i < scenes; i++)
				pointers[i] = (u8*)table + p2_read_big<u32>(table + 1 + i);
			JAIBasic::getInterface()->_1C = pointers;
			at += 3;
			break;
		}
		case 7:
			Fx::initOnCodeFxScene = (Fx::Init*)transInitDataFile((u8*)aafPointer + aafWord(at), aafWord(at + 1));
			at += 3;
			break;
		case 8:
			JAIBasic::getInterface()->mRawDataPtr = transInitDataFile((u8*)aafPointer + aafWord(at), (aafWord(at + 1) & 0xFFF0) + 16);
			at += 3;
			break;
		default:
			while (aafWord(at++)) { }
			break;
		}
	}
	BankWave::initCallback();""")
    for kind, record, field, extra in (("Bnk", "TCodeBnk", "mBankData", "\t\tinitOnCodeBnk[i].mWaveBankId = aafWord(*p1 + 2);\n"),
                                        ("Ws", "TCodeWS", "_00", "\t\tinitOnCodeWs[i]._08 = aafWord(*p1 + 2);\n\t\tBankWave::wsMax++;\n")):
        param = "p1" if kind == "Bnk" else "data"
        body = f"""\tu32 count = 0;
	while (aafWord(*{param} + count * 3)) count++;
	BankWave::initOnCode{kind} = new (JAIBasic::msCurrentHeap, 0x20) BankWave::{record}[count + 1]();
	for (u32 i = 0; i < count; i++, *{param} += 3) {{
		BankWave::initOnCode{kind}[i].{field} = (int*)((u8*)aafPointer + aafWord(*{param}));
		BankWave::initOnCode{kind}[i]._04 = aafWord(*{param} + 1);
""" + extra.replace("initOnCode", "BankWave::initOnCode").replace("*p1", "*" + param) + f"""\t}}
	*{param} += 1;"""
        native_body("src/JSystem/JAudio/JAI/JAIInitData.cpp", "JAInter::InitData::init" + kind + "List", body)
    # The FX scene block is used in place: big-endian words, then scene offsets.
    replace("include/JSystem/JAudio/JAI/JAInter/Fx.h",
            "u32 mSceneMax;             // _00\n\tu32 mBufferMax1;           // _04\n\tu32 mBufferMax2;           // _08\n"
            "\tu32 mBufferMax3;           // _0C\n\tu32 mBufferMax4;           // _10\n\ts32* mScenePointerOffsets; // _14",
            "P2Big<u32> mSceneMax;   // _00\n\tP2Big<u32> mBufferMax1;  // _04\n\tP2Big<u32> mBufferMax2;  // _08\n"
            "\tP2Big<u32> mBufferMax3;  // _0C\n\tP2Big<u32> mBufferMax4;  // _10\n\tP2Big<s32> mScenePointerOffsets[1]; // _14, one per scene")
    replace("include/JSystem/JAudio/JAI/JAInter/Fx.h", '#include "types.h"', '#include "types.h"\n#include "p2_endian.h"')
    replace("src/JSystem/JAudio/JAI/JAIFx.cpp",
            "setScenePointer(i, (reinterpret_cast<u8*>(initOnCodeFxScene) + *(int*)((u8*)(&init->mScenePointerOffsets + i))));",
            "setScenePointer(i, reinterpret_cast<u8*>(initOnCodeFxScene) + s32(init->mScenePointerOffsets[i]));")

    # Pikmin 2's bank manager keeps the resident AAF's bank and wave-system
    # lists as 12-byte records whose first word the console relocated into a
    # pointer. Native records hold a host pointer.
    replace("include/PSSystem/BankMgr.h", "struct PSInstData {\n\tu32 _00;", "struct PSInstData {\n\tvoid* _00; // AAF data (console: relocated offset)")
    for name, member, count in (("setBankData", "mBankData", "mInstBankNum"), ("setWsData", "mWsData", "mWaveBankNum")):
        native_body("src/plugProjectHikinoU/PSBnkMgr.cpp", "BankMgr::" + name, f"""\tu32* aafPtr = JAInter::InitData::aafPointer;
	P2ASSERTLINE(56, aafPtr);
	P2ASSERTLINE(57, !{member});
	u32 records = 0;
	while (p2_read_big<u32>(aafPtr + data[0] + records * 3)) records++;
	{member} = new (JASDram, 0x20) PSInstData[records + 1]();
	for (u32 i = 0; i < records; i++) {{
		const u32* record = aafPtr + data[0] + i * 3;
		{member}[i]._00   = (u8*)aafPtr + p2_read_big<u32>(record);
		{member}[i]._04   = p2_read_big<u32>(record + 1);
		{member}[i].mIndex = p2_read_big<u32>(record + 2);
	}}
	{count} = records;
	data[0] += records * 3 + 1;""")
    replace("src/plugProjectHikinoU/PSBnkMgr.cpp", "\t\t\t// this does NOT feel right, but it matches\n\t\t\tPSInstData** ptr = &((PSInstData**)mWsData)[i * 3];\n\t\t\tif (*ptr) {\n\t\t\t\tJASWaveBankMgr::registWaveBankWS(i, *ptr);",
            "\t\t\tif (mWsData[i]._00) {\n\t\t\t\tJASWaveBankMgr::registWaveBankWS(i, mWsData[i]._00);")
    replace("src/plugProjectHikinoU/PSBnkMgr.cpp", "\t\t\tPSInstData** ptr = &((PSInstData**)mBankData)[i * 3];\n\t\t\tif (*ptr) {\n\t\t\t\tJASBankMgr::registBankBNK((u32)i, *ptr);",
            "\t\t\tif (mBankData[i]._00) {\n\t\t\t\tJASBankMgr::registBankBNK((u32)i, mBankData[i]._00);")
    replace("src/plugProjectHikinoU/PSBnkMgr.cpp", '#include "JSystem/JAudio/JAS/JASDvd.h"', '#include "JSystem/JAudio/JAS/JASDvd.h"\n#include "p2_endian.h"')

    # The audio thread renders on the host (src/audio_host.cpp) instead of
    # waiting for AI DMA and DSP interrupt messages.
    file = destination / "src/JSystem/JAudio/JAS/JASAudioThread.cpp"
    text = sub_exact(r"\tJASDriver::startDMA\(\);\n\n\twhile \(true\) \{.*?\n\t\}\n\}", "\tJASDriver::startDMA();\n\tp2_audio_host_run();\n\treturn nullptr;\n}",
                     file.read_text(), 1, "JASAudioThread::run loop", flags=re.S)
    file.write_text('extern "C" void p2_audio_host_run();\n' + text)

    # Instrument banks (IBNK) and wave systems (WSYS) are parsed in place into
    # native objects; their disk fields are big-endian.
    bnk = "include/JSystem/JAudio/JAS/JASBNKParser.h"
    replace(bnk, '#include "types.h"', '#include "types.h"\n#include "p2_endian.h"')
    replace(bnk, "u32 mOffset; // _00", "P2Big<u32> mOffset; // _00")
    text = (destination / bnk).read_text()
    text = sub_exact(r"\b(u32|u16|f32) (mMagic|mVolume|mPitch|mKeyRegionCount|mVelRegCount|mRate|mWidth|mVertex|"
                     r"mRelease|mVeloRegionCount|mFloor|mCeiling|mWaveID)\b", r"P2Big<\1> \2", text, 20, bnk)
    (destination / bnk).write_text(text)
    # Oscillator envelope tables (s16 mode/time/value triples) are copied and
    # then read natively, so the copy converts them.
    replace("src/JSystem/JAudio/JAS/JASBNKParser.cpp", "JASCalc::bcopy(oscTable, tableCopy, tableLength);",
            "for (u32 k = 0; k < tableLength / sizeof(s16); k++)\n\t\t\t\t\t\t\t\treinterpret_cast<s16*>(tableCopy)[k] = p2_read_big<s16>(oscTable + k);", count=2)
    replace("src/JSystem/JAudio/JAS/JASBNKParser.cpp", "\t\tv1 = *p1;\n", "\t\tv1 = p2_read_big<s16>(p1);\n")
    wave = "include/JSystem/JAudio/JAS/JASWave.h"
    replace(wave, "struct TOffset {\n\tT* ptr(const void* base) const { return JSUConvertOffsetToPtr<T>(base, mOffset); }\n\tu32 mOffset;",
            "struct TOffset {\n\tT* ptr(const void* base) const { return JSUConvertOffsetToPtr<T>(base, mOffset); }\n\tP2Big<u32> mOffset;")
    replace(wave, "struct TCtrlWave {\n\tu32 _00; // _00", "struct TCtrlWave {\n\tP2Big<u32> _00; // _00")
    for old, new in (("f32 mSampleRate;  // _04", "P2Big<f32> mSampleRate; // _04"), ("u32 mAwOffset;    // _08", "P2Big<u32> mAwOffset; // _08"),
                     ("u32 mAwLength;    // _0C", "P2Big<u32> mAwLength; // _0C"), ("u32 mLoop;        // _10", "P2Big<u32> mLoop; // _10"),
                     ("u32 mLoopStart;   // _14", "P2Big<u32> mLoopStart; // _14"), ("u32 mLoopEnd;     // _18", "P2Big<u32> mLoopEnd; // _18"),
                     ("u32 mSampleCount; // _1C", "P2Big<u32> mSampleCount; // _1C"), ("s16 mLast;        // _20", "P2Big<s16> mLast; // _20"),
                     ("s16 mPenult;      // _22", "P2Big<s16> mPenult; // _22"),
                     ("u32 mWaveCount;                         // _04", "P2Big<u32> mWaveCount; // _04"),
                     ("u32 mCtrlGroupCount;                      // _08", "P2Big<u32> mCtrlGroupCount; // _08")):
        replace(wave, old, new)
    replace(wave, '#include "JSystem/JSupport/JSU.h"', '#include "JSystem/JSupport/JSU.h"\n#include "p2_endian.h"')

    # Task-thread commands: run() reads a JASThreadCallStack {function, flag,
    # payload}. The decomp's copying allocator wrote a different header and the
    # pointer variant allocated 12 bytes; both are sized for the host layout.
    task = "src/JSystem/JAudio/JAS/JASTaskThread.cpp"
    replace(task, """	// UNUSED FUNCTION
	// TODO: Wrong.
	size_t fullLength          = msgLength + 8;
	JASCmdHeap::Header* header = (JASCmdHeap::Header*)JASKernel::getCommandHeap()->alloc(fullLength);
	if (header == nullptr) {
		return nullptr;
	}
	header->mMsgLength = 1;
	JASCalc::bcopy(msg, header + 1, msgLength);
	header->mCommand = cmd;
	return header;""", """	JASThreadCallStack* callStack
	    = (JASThreadCallStack*)JASKernel::getCommandHeap()->alloc(offsetof(JASThreadCallStack, mByteBuffer) + msgLength);
	if (callStack == nullptr) {
		return nullptr;
	}
	callStack->mRunFunc = cmd;
	callStack->_04      = 1; // payload copied into the call stack
	memcpy(callStack->mByteBuffer, msg, msgLength);
	return (JASCmdHeap::Header*)callStack;""")
    replace(task, "callStack = (JASThreadCallStack*)heap->alloc(0xc);",
            "callStack = (JASThreadCallStack*)heap->alloc(offsetof(JASThreadCallStack, mVoidBuffer) + sizeof(void*));")
    replace(task, '#include "JSystem/JKernel/JKRThread.h"', '#include "JSystem/JKernel/JKRThread.h"\n#include <stddef.h>\n#include <string.h>')

    # Cutscene sound objects (JStudio_JAudio): paragraph payloads are big-endian.
    replace("src/JSystem/JStudio_JAudio/object-sound.cpp", "*(f32*)data", "p2_read_big<f32>(data)", count=2)
    replace("src/JSystem/JStudio_JAudio/object-sound.cpp", "_100 = *(int*)data;", "_100 = p2_read_big<s32>(data);")
    # A 32-bit "has position" flag in the file, read natively as an 8-byte pointer.
    replace("src/JSystem/JStudio_JAudio/object-sound.cpp", "\t\tVec* pos  = *(Vec**)data;\n\t\tmPosition = nullptr;\n\t\tif (!pos)", "\t\tconst bool located = p2_read_big<u32>(data) != 0;\n\t\tmPosition = nullptr;\n\t\tif (!located)")
    for setter in ("setDemoVolume", "setDemoPan", "setDemoPitch", "setTempoProportion", "setDemoFxmix"):
        replace("src/JSystem/JStudio_JAudio/object-sound.cpp", ", JAISound::" + setter + ")", ", &JAISound::" + setter + ")")
    # inherit()'s result is unused (openChild); MWCC fell off the end.
    replace("src/JSystem/JAudio/JAS/JASTrack.cpp",
            "\t\tmChannelUpdater.mDolbyCalcType = mParentTrack->mChannelUpdater.mDolbyCalcType;\n\t}\n}",
            "\t\tmChannelUpdater.mDolbyCalcType = mParentTrack->mChannelUpdater.mDolbyCalcType;\n\t}\n\treturn 0;\n}")
    # The channel pool was sized with the console object size.
    replace("src/JSystem/JAudio/JAS/JASAudioThread.cpp", "->newMemPool(0x118, 0x48);", "->newMemPool(sizeof(JASChannel), 0x48);")
    replace("src/JSystem/JAudio/JAS/JASWaveArcLoader.cpp", "sendCmdMsg(loadToAramCallback, &args, 0x10)",
            "sendCmdMsg(loadToAramCallback, &args, sizeof(args))")

    # Sound table (BST, AAF section 1) is used in place: 16-byte big-endian
    # SoundInfo records after a category header.
    replace("include/JSystem/JAudio/JAI/JAInter.h", "struct SoundInfo {\n\tu32 mFlag;     // _00",
            "struct SoundInfo {\n\tP2Big<u32> mFlag; // _00")
    replace("include/JSystem/JAudio/JAI/JAInter.h", "\tu16 mOffsetNo; // _06\n\tf32 mPitch;    // _08",
            "\tP2Big<u16> mOffsetNo; // _06\n\tP2Big<f32> mPitch;    // _08")
    replace("src/JSystem/JAudio/JAI/JAISoundTable.cpp", "mSoundMax[i]        = *reinterpret_cast<u16*>(&mAddress[6 + i * 4]);",
            "mSoundMax[i]        = p2_read_big<u16>(&mAddress[6 + i * 4]);")
    replace("src/JSystem/JAudio/JAI/JAISoundTable.cpp", "mPointerCategory[i] = &((SoundInfo*)&mAddress[0x50])[*(u16*)(&mAddress[8 + i * 4])];",
            "mPointerCategory[i] = &((SoundInfo*)&mAddress[0x50])[p2_read_big<u16>(&mAddress[8 + i * 4])];")
    replace("include/JSystem/JAudio/JAI/JAInter.h", '#include "JSystem/JAudio/JAI/JAInter/MoveParaSet.h"',
            '#include "JSystem/JAudio/JAI/JAInter/MoveParaSet.h"\n#include "p2_endian.h"')
    # Stream list (AAF section 5): big-endian u16 offsets to file names.
    replace("src/JSystem/JAudio/JAI/JAIStreamMgr.cpp",
            "(streamList[JAIBasic::getInterface()->getSoundOffsetNumberFromID(streamSound->mSoundID) + 2])",
            "p2_read_big<u16>(&streamList[JAIBasic::getInterface()->getSoundOffsetNumberFromID(streamSound->mSoundID) + 2])")
    replace("src/JSystem/JAudio/JAI/JAIStreamMgr.cpp", "u16* streamList;", "#include \"p2_endian.h\"\nu16* streamList;")

    # AST stream files: big-endian header (STRM) and per-block BLCK headers.
    stream = "src/JSystem/JAudio/JAS/JASAramStream.cpp"
    text = (destination / stream).read_text()
    text = sub_exact(r"\*\(u16\*\)\(buffer \+ (0x[0-9A-Fa-f]+)\)", r"p2_read_big<u16>(buffer + \1)", text, 3, stream + " u16")
    text = sub_exact(r"\*\(u32\*\)\(buffer \+ (0x[0-9A-Fa-f]+)\)", r"p2_read_big<u32>(buffer + \1)", text, 3, stream + " u32")
    text = sub_exact(r"preBuffer\[1\]", "p2_read_big<u32>(preBuffer + 1)", text, 2, stream + " block size")
    text = sub_exact(r"\(\(s16\*\)preBuffer\)\[(2 \* i \+ [45])\]", r"p2_read_big<s16>((s16*)preBuffer + \1)", text, 2, stream + " history")
    (destination / stream).write_text('#include "p2_endian.h"\n' + text)

    # Animation sound tables (BAS) are read in place.
    anime = "include/JSystem/JAudio/JAI/JAIAnimeSound.h"
    text = (destination / anime).read_text()
    text = sub_exact(r"\b(u32|f32|u16) (mSoundID|mStartTime|mEndTime|mPitch|mPlayFlags|mEntryNum|_02|_04);",
                     r"P2Big<\1> \2;", text, 8, anime)
    (destination / anime).write_text('#include "p2_endian.h"\n' + text)
    replace("src/JSystem/JStudio_JAudio/object-sound.cpp", "mFlags = *(int*)data;", "mFlags = p2_read_big<s32>(data);")
    replace("src/JSystem/JStudio_JAudio/object-sound.cpp", "_104 = *(int*)data;", "_104 = p2_read_big<s32>(data);")
