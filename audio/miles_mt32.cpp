/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include "audio/miles.h"

#include "common/config-manager.h"
#include "common/file.h"
#include "common/mutex.h"
#include "common/system.h"
#include "common/textconsole.h"

namespace Audio {

// Miles Audio MT32 driver
//
// TODO: currently missing: timbre file support (used in 7th Guest)

#define MILES_MT32_PATCHES_COUNT 128
#define MILES_MT32_CUSTOMTIMBRE_COUNT 64

#define MILES_MT32_PATCHDATA_COMMONPARAMETER_SIZE 14
#define MILES_MT32_PATCHDATA_PARTIALPARAMETER_SIZE 58
#define MILES_MT32_PATCHDATA_PARTIALPARAMETERS_COUNT 4
#define MILES_MT32_PATCHDATA_TOTAL_SIZE (MILES_MT32_PATCHDATA_COMMONPARAMETER_SIZE + (MILES_MT32_PATCHDATA_PARTIALPARAMETER_SIZE * MILES_MT32_PATCHDATA_PARTIALPARAMETERS_COUNT))

struct MilesMT32InstrumentEntry {
	byte bankId;
	byte patchId;
	byte commonParameter[MILES_MT32_PATCHDATA_COMMONPARAMETER_SIZE + 1];
	byte partialParameters[MILES_MT32_PATCHDATA_PARTIALPARAMETERS_COUNT][MILES_MT32_PATCHDATA_PARTIALPARAMETER_SIZE + 1];
};

const byte milesMT32SysExResetParameters[] = {
	0x01, 0xFF
};

const byte milesMT32SysExChansSetup[] = {
	0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0xFF
};

const byte milesMT32SysExPartialReserveTable[] = {
	0x03, 0x04, 0x03, 0x04, 0x03, 0x04, 0x03, 0x04, 0x04, 0xFF
};

const byte milesMT32SysExInitReverb[] = {
	0x00, 0x03, 0x02, 0xFF // Reverb mode 0, reverb time 3, reverb level 2
};

class MidiDriver_Miles_MT32 : public MidiDriver {
public:
	MidiDriver_Miles_MT32(MilesMT32InstrumentEntry *instrumentTablePtr, uint16 instrumentTableCount);
	virtual ~MidiDriver_Miles_MT32();

	// MidiDriver
	int open();
	void close();
	bool isOpen() const { return _isOpen; }

	void send(uint32 b);

	MidiChannel *allocateChannel() {
		if (_driver)
			return _driver->allocateChannel();
		return NULL;
	}
	MidiChannel *getPercussionChannel() {
		if (_driver)
			return _driver->getPercussionChannel();
		return NULL;
	}

	void setTimerCallback(void *timer_param, Common::TimerManager::TimerProc timer_proc) {
		if (_driver)
			_driver->setTimerCallback(timer_param, timer_proc);
	}

	uint32 getBaseTempo() {
		if (_driver) {
			return _driver->getBaseTempo();
		}
		return 1000000 / _baseFreq;
	}

protected:
	Common::Mutex _mutex;
	MidiDriver *_driver;
	bool _MT32;
	bool _nativeMT32;

	bool _isOpen;
	int _baseFreq;

public:

private:
	void resetMT32();

	void MT32SysEx(const uint32 targetAddress, const byte *dataPtr);

	void writeRhythmSetup(byte note, byte customTimbreId);
	void writePatchTimbre(byte patchId, byte timbreGroup, byte timbreId);
	void writePatchByte(byte patchId, byte index, byte patchValue);
	void writeToSystemArea(byte index, byte value);

	void controlChange(byte midiChannel, byte controllerNumber, byte controllerValue);
	void programChange(byte midiChannel, byte patchId);

	const MilesMT32InstrumentEntry *searchCustomInstrument(byte patchBank, byte patchId);
	int16 searchCustomTimbre(byte patchBank, byte patchId);

	void setupPatch(byte patchBank, byte patchId);
	int16 installCustomTimbre(byte patchBank, byte patchId);

private:
	struct MidiChannelEntry {
		byte   currentPatchBank;
		byte   currentPatchId;

		bool   usingCustomTimbre;
		byte   currentCustomTimbreId;

		MidiChannelEntry() : currentPatchBank(0),
							currentPatchId(0),
							usingCustomTimbre(false),
							currentCustomTimbreId(0) { }
	};

	struct MidiCustomTimbreEntry {
		bool   used;
		bool   protectionEnabled;
		byte   currentPatchBank;
		byte   currentPatchId;

		uint32 lastUsedNoteCounter;

		MidiCustomTimbreEntry() : used(false),
								protectionEnabled(false),
								currentPatchBank(0),
								currentPatchId(0),
								lastUsedNoteCounter(0) {}
	};

	// stores information about all MIDI channels
	MidiChannelEntry _midiChannels[MILES_MIDI_CHANNEL_COUNT];

	// stores information about all custom timbres
	MidiCustomTimbreEntry _customTimbres[MILES_MT32_CUSTOMTIMBRE_COUNT];

	byte _patchesBank[MILES_MT32_PATCHES_COUNT];

	// holds all instruments
	MilesMT32InstrumentEntry *_instrumentTablePtr;
	uint16                   _instrumentTableCount;

	uint32           _noteCounter; // used to figure out, which timbres are outdated
};

MidiDriver_Miles_MT32::MidiDriver_Miles_MT32(MilesMT32InstrumentEntry *instrumentTablePtr, uint16 instrumentTableCount) {
	_instrumentTablePtr = instrumentTablePtr;
	_instrumentTableCount = instrumentTableCount;

	_driver = NULL;
	_isOpen = false;
	_MT32 = false;
	_nativeMT32 = false;
	_baseFreq = 250;

	_noteCounter = 0;

	memset(_patchesBank, 0, sizeof(_patchesBank));
}

MidiDriver_Miles_MT32::~MidiDriver_Miles_MT32() {
	Common::StackLock lock(_mutex);
	if (_driver) {
		_driver->setTimerCallback(0, 0);
		_driver->close();
		delete _driver;
	}
	_driver = NULL;
}

int MidiDriver_Miles_MT32::open() {
	assert(!_driver);

	// Setup midi driver
	MidiDriver::DeviceHandle dev = MidiDriver::detectDevice(MDT_MIDI | MDT_PREFER_MT32);
	MusicType musicType = MidiDriver::getMusicType(dev);

	switch (musicType) {
	case MT_MT32:
		_nativeMT32 = true;
		break;
	case MT_GM:
		if (ConfMan.getBool("native_mt32")) {
			_nativeMT32 = true;
		}
		break;
	default:
		break;
	}

	if (!_nativeMT32) {
		error("MILES-MT32: non-mt32 currently not supported!");
	}

	_driver = MidiDriver::createMidi(dev);
	if (!_driver)
		return 255;

	if (_nativeMT32)
		_driver->property(MidiDriver::PROP_CHANNEL_MASK, 0x03FE);

	int ret = _driver->open();
	if (ret)
		return ret;

	if (_nativeMT32) {
		_driver->sendMT32Reset();

		resetMT32();
	}

	return 0;
}

void MidiDriver_Miles_MT32::close() {
	if (_driver) {
		_driver->close();
	}
}

void MidiDriver_Miles_MT32::resetMT32() {
	// reset all internal parameters / patches
	MT32SysEx(0x7F0000, milesMT32SysExResetParameters);

	// init part/channel assignments
	MT32SysEx(0x10000D, milesMT32SysExChansSetup);

	// partial reserve table
	MT32SysEx(0x100004, milesMT32SysExPartialReserveTable);

	// init reverb
	MT32SysEx(0x100001, milesMT32SysExInitReverb);
}

void MidiDriver_Miles_MT32::MT32SysEx(const uint32 targetAddress, const byte *dataPtr) {
	byte   sysExMessage[270];
	uint16 sysExPos      = 0;
	byte   sysExByte     = 0;
	uint16 sysExChecksum = 0;

	memset(&sysExMessage, 0, sizeof(sysExMessage));

	sysExMessage[0] = 0x41; // Roland
	sysExMessage[1] = 0x10;
	sysExMessage[2] = 0x16; // Model MT32
	sysExMessage[3] = 0x12; // Command DT1

	sysExChecksum = 0;

	sysExMessage[4] = (targetAddress >> 16) & 0xFF;
	sysExMessage[5] = (targetAddress >> 8) & 0xFF;
	sysExMessage[6] = targetAddress & 0xFF;

	sysExChecksum -= sysExMessage[4];
	sysExChecksum -= sysExMessage[5];
	sysExChecksum -= sysExMessage[6];

	sysExPos      = 7;
	while (1) {
		sysExByte = *dataPtr++;
		if (sysExByte == 0xff)
			break; // Message done

		assert(sysExPos < sizeof(sysExMessage));
		sysExMessage[sysExPos++] = sysExByte;
		sysExChecksum -= sysExByte;
	}

	// Calculate checksum
	assert(sysExPos < sizeof(sysExMessage));
	sysExMessage[sysExPos++] = sysExChecksum & 0x7f;

	// Send SysEx
	_driver->sysEx(sysExMessage, sysExPos);

	// Wait the time it takes to send the SysEx data
	uint32 delay = (sysExPos + 2) * 1000 / 3125;

	// Plus an additional delay for the MT-32 rev00
	if (_nativeMT32)
		delay += 40;

	g_system->delayMillis(delay);
}

// MIDI messages can be found at http://www.midi.org/techspecs/midimessages.php
void MidiDriver_Miles_MT32::send(uint32 b) {
	byte command = b & 0xf0;
	byte midiChannel = b & 0xf;
	byte op1 = (b >> 8) & 0xff;
	byte op2 = (b >> 16) & 0xff;

	switch (command) {
	case 0x80: // note off
	case 0x90: // note on
	case 0xa0: // Polyphonic key pressure (aftertouch)
	case 0xd0: // Channel pressure (aftertouch)
	case 0xe0: // pitch bend change
		_noteCounter++;
		if (_midiChannels[midiChannel].usingCustomTimbre) {
			// Remember that this timbre got used now
			_customTimbres[_midiChannels[midiChannel].currentCustomTimbreId].lastUsedNoteCounter = _noteCounter;
		}
		_driver->send(b);
		break;
	case 0xb0: // Control change
		controlChange(midiChannel, op1, op2);
		break;
	case 0xc0: // Program Change
		programChange(midiChannel, op1);
		break;
	case 0xf0: // SysEx
		warning("MILES-MT32: SysEx: %x", b);
		break;
	default:
		warning("MILES-MT32: Unknown event %02x", command);
	}
}

void MidiDriver_Miles_MT32::controlChange(byte midiChannel, byte controllerNumber, byte controllerValue) {
	byte channelPatchId = 0;
	byte channelCustomTimbreId = 0;

	switch (controllerNumber) {
	case MILES_CONTROLLER_SELECT_PATCH_BANK:
		_midiChannels[midiChannel].currentPatchBank = controllerValue;
		return;

	case MILES_CONTROLLER_PATCH_REVERB:
		channelPatchId = _midiChannels[midiChannel].currentPatchId;

		writePatchByte(channelPatchId, 6, controllerValue);
		_driver->send(0xC0 | midiChannel | (channelPatchId << 8)); // execute program change
		return;

	case MILES_CONTROLLER_PATCH_BENDER:
		channelPatchId = _midiChannels[midiChannel].currentPatchId;

		writePatchByte(channelPatchId, 4, controllerValue);
		_driver->send(0xC0 | midiChannel | (channelPatchId << 8)); // execute program change
		return;

	case MILES_CONTROLLER_REVERB_MODE:
		writeToSystemArea(1, controllerValue);
		return;

	case MILES_CONTROLLER_REVERB_TIME:
		writeToSystemArea(2, controllerValue);
		return;

	case MILES_CONTROLLER_REVERB_LEVEL:
		writeToSystemArea(3, controllerValue);
		return;

	case MILES_CONTROLLER_RHYTHM_KEY_TIMBRE:
		if (_midiChannels[midiChannel].usingCustomTimbre) {
			// custom timbre is set on current channel
			writeRhythmSetup(controllerValue, _midiChannels[midiChannel].currentCustomTimbreId);
		}
		return;

	case MILES_CONTROLLER_PROTECT_TIMBRE:
		if (_midiChannels[midiChannel].usingCustomTimbre) {
			// custom timbre set on current channel
			channelCustomTimbreId = _midiChannels[midiChannel].currentCustomTimbreId;
			if (controllerValue >= 64) {
				// enable protection
				_customTimbres[channelCustomTimbreId].protectionEnabled = true;
			} else {
				// disable protection
				_customTimbres[channelCustomTimbreId].protectionEnabled = false;
			}
		}
		return;

	default:
		break;
	}

	if ((controllerNumber >= MILES_CONTROLLER_SYSEX_RANGE_BEGIN) && (controllerNumber <= MILES_CONTROLLER_SYSEX_RANGE_END)) {
		// send SysEx
		warning("MILES-MT32: embedded SysEx controller %2x, value %2x", controllerNumber, controllerValue);
		return;
	}

	if ((controllerNumber >= MILES_CONTROLLER_XMIDI_RANGE_BEGIN) && (controllerNumber <= MILES_CONTROLLER_XMIDI_RANGE_END)) {
		// XMIDI controllers? ignore those
		return;
	}

	_driver->send(0xB0 | midiChannel | (controllerNumber << 8) | (controllerValue << 16));
}

void MidiDriver_Miles_MT32::programChange(byte midiChannel, byte patchId) {
	byte channelPatchBank = _midiChannels[midiChannel].currentPatchBank;
	byte activePatchBank = _patchesBank[patchId];

	// remember patch id for the current MIDI-channel
	_midiChannels[midiChannel].currentPatchId = patchId;

	if (channelPatchBank != activePatchBank) {
		// associate patch with timbre
		setupPatch(channelPatchBank, patchId);
	}

	// If this is a custom patch, remember customTimbreId
	int16 customTimbre = searchCustomTimbre(channelPatchBank, patchId);
	if (customTimbre >= 0) {
		_midiChannels[midiChannel].usingCustomTimbre = true;
		_midiChannels[midiChannel].currentCustomTimbreId = customTimbre;
	} else {
		_midiChannels[midiChannel].usingCustomTimbre = false;
	}

	// Finally send program change to MT32
	_driver->send(0xC0 | midiChannel | (patchId << 8));
}

int16 MidiDriver_Miles_MT32::searchCustomTimbre(byte patchBank, byte patchId) {
	byte customTimbreId = 0;

	for (customTimbreId = 0; customTimbreId < MILES_MT32_CUSTOMTIMBRE_COUNT; customTimbreId++) {
		if (_customTimbres[customTimbreId].used) {
			if ((_customTimbres[customTimbreId].currentPatchBank == patchBank) && (_customTimbres[customTimbreId].currentPatchId == patchId)) {
				return customTimbreId;
			}
		}
	}
	return -1;
}

const MilesMT32InstrumentEntry *MidiDriver_Miles_MT32::searchCustomInstrument(byte patchBank, byte patchId) {
	const MilesMT32InstrumentEntry *instrumentPtr = _instrumentTablePtr;

	for (uint16 instrumentNr = 0; instrumentNr < _instrumentTableCount; instrumentNr++) {
		if ((instrumentPtr->bankId == patchBank) && (instrumentPtr->patchId == patchId))
			return instrumentPtr;
	}
	return NULL;
}

void MidiDriver_Miles_MT32::setupPatch(byte patchBank, byte patchId) {
	_patchesBank[patchId] = patchBank;

	if (patchBank) {
		// non-built-in bank
		int16 customTimbreId = searchCustomTimbre(patchBank, patchId);
		if (customTimbreId < 0) {
			// currently not loaded, try to install it
			// Miles Audio didn't do this here, I'm not exactly sure when it called the install code
			customTimbreId = installCustomTimbre(patchBank, patchId);
		}
		if (customTimbreId >= 0) {
			// now available? -> use this timbre
			writePatchTimbre(patchId, 2, customTimbreId); // Group MEMORY
			return;
		}
	}

	// for built-in bank (or timbres, that are not available) use default MT32 timbres
	byte timbreId = patchId & 0x3F;
	if (!(patchId & 0x40)) {
		writePatchTimbre(patchId, 0, timbreId); // Group A
	} else {
		writePatchTimbre(patchId, 1, timbreId); // Group B
	}
}

//
int16 MidiDriver_Miles_MT32::installCustomTimbre(byte patchBank, byte patchId) {
	switch(patchBank) {
	case 0:   // Standard Roland MT32 bank
	case 127: // Reserved for melodic mode
		return -1;
	default:
		break;
	}

	// Original driver did a search for custom timbre here
	// and in case it was found, it would call setup_patch()
	// we are called from within setup_patch(), so this isn't needed

	int16 customTimbreId = -1;
	int16 leastUsedTimbreId = -1;
	uint32 leastUsedTimbreNoteCounter = _noteCounter;
	const MilesMT32InstrumentEntry *instrumentPtr = NULL;

	// Check, if requested instrument is actually available
	instrumentPtr = this->searchCustomInstrument(patchBank, patchId);
	if (!instrumentPtr) {
		return -1; // not found -> bail out
	}

	// Look for an empty timbre slot
	// or get the least used non-protected slot
	for (byte customTimbreNr = 0; customTimbreNr < MILES_MT32_CUSTOMTIMBRE_COUNT; customTimbreNr++) {
		if (!_customTimbres[customTimbreNr].used) {
			// found an empty slot -> use this one
			customTimbreId = customTimbreNr;
			break;
		} else {
			// used slot
			if (!_customTimbres[customTimbreNr].protectionEnabled) {
				// not protected
				uint32 customTimbreNoteCounter = _customTimbres[customTimbreNr].lastUsedNoteCounter;
				if (customTimbreNoteCounter < leastUsedTimbreNoteCounter) {
					leastUsedTimbreId          = customTimbreNr;
					leastUsedTimbreNoteCounter = customTimbreNoteCounter;
				}
			}
		}
	}

	if (customTimbreId < 0) {
		// no empty slot found, check if we got a least used non-protected slot
		if (leastUsedTimbreId < 0) {
			// everything is protected, bail out
			return -1;
		}
		customTimbreId = leastUsedTimbreId;
	}

	// setup timbre slot
	_customTimbres[customTimbreId].used                = true;
	_customTimbres[customTimbreId].currentPatchBank    = patchBank;
	_customTimbres[customTimbreId].currentPatchId      = patchId;
	_customTimbres[customTimbreId].lastUsedNoteCounter = _noteCounter;
	_customTimbres[customTimbreId].protectionEnabled   = false;

	uint32 targetAddress = 0x080000 | (customTimbreId << 9);
	uint32 targetAddressCommon   = targetAddress + 0x000000;
	uint32 targetAddressPartial1 = targetAddress + 0x00000E;
	uint32 targetAddressPartial2 = targetAddress + 0x000048;
	uint32 targetAddressPartial3 = targetAddress + 0x000102;
	uint32 targetAddressPartial4 = targetAddress + 0x00013C;

	// upload common parameter data
	MT32SysEx(targetAddressCommon, instrumentPtr->commonParameter);
	// upload partial parameter data
	MT32SysEx(targetAddressPartial1, instrumentPtr->partialParameters[0]);
	MT32SysEx(targetAddressPartial2, instrumentPtr->partialParameters[1]);
	MT32SysEx(targetAddressPartial3, instrumentPtr->partialParameters[2]);
	MT32SysEx(targetAddressPartial4, instrumentPtr->partialParameters[3]);

	return customTimbreId;
}

void MidiDriver_Miles_MT32::writeRhythmSetup(byte note, byte customTimbreId) {
	byte   sysExData[2];
	uint32 targetAddress = 0;

	targetAddress = 0x030110 + ((note - 24) << 2);

	sysExData[0] = customTimbreId;
	sysExData[1] = 0xFF; // terminator

	MT32SysEx(targetAddress, sysExData);
}

void MidiDriver_Miles_MT32::writePatchTimbre(byte patchId, byte timbreGroup, byte timbreId) {
	byte   sysExData[3];
	uint32 targetAddress = 0;

	targetAddress = ((patchId << 3) << 16) | 0x000500;

	sysExData[0] = timbreGroup;
	sysExData[1] = timbreId;
	sysExData[2] = 0xFF; // terminator

	MT32SysEx(targetAddress, sysExData);
}

void MidiDriver_Miles_MT32::writePatchByte(byte patchId, byte index, byte patchValue) {
	byte   sysExData[2];
	uint32 targetAddress = 0;

	targetAddress = (((patchId << 3) + index ) << 16) | 0x000500;

	sysExData[0] = patchValue;
	sysExData[1] = 0xFF; // terminator

	MT32SysEx(targetAddress, sysExData);
}

void MidiDriver_Miles_MT32::writeToSystemArea(byte index, byte value) {
	byte   sysExData[2];
	uint32 targetAddress = 0;

	targetAddress = 0x100000 | index;

	sysExData[0] = value;
	sysExData[1] = 0xFF; // terminator

	MT32SysEx(targetAddress, sysExData);
}

MidiDriver *MidiDriver_Miles_MT32_create(const Common::String instrumentDataFilename) {
	MilesMT32InstrumentEntry *instrumentTablePtr = NULL;
	uint16                    instrumentTableCount = 0;

	if (!instrumentDataFilename.empty()) {
		// Load MT32 instrument data from file SAMPLE.MT
		Common::File *fileStream = new Common::File();
		uint32        fileSize = 0;
		byte         *fileDataPtr = NULL;
		uint32        fileDataOffset = 0;
		uint32        fileDataLeft = 0;

		byte curBankId = 0;
		byte curPatchId = 0;

		MilesMT32InstrumentEntry *instrumentPtr = NULL;
		uint32                    instrumentOffset = 0;
		uint16                    instrumentDataSize = 0;

		if (!fileStream->open(instrumentDataFilename))
			error("MILES-MT32: could not open instrument file '%s'", instrumentDataFilename.c_str());

		fileSize = fileStream->size();

		fileDataPtr = new byte[fileSize];

		if (fileStream->read(fileDataPtr, fileSize) != fileSize)
			error("MILES-MT32: error while reading instrument file");
		fileStream->close();
		delete fileStream;

		// File is like this:
		// [patch:BYTE] [bank:BYTE] [patchoffset:UINT32]
		// ...
		// until patch + bank are both 0xFF, which signals end of header

		// First we check how many entries there are
		fileDataOffset = 0;
		fileDataLeft = fileSize;
		while (1) {
			if (fileDataLeft < 6)
				error("MILES-MT32: unexpected EOF in instrument file");

			curPatchId = fileDataPtr[fileDataOffset++];
			curBankId  = fileDataPtr[fileDataOffset++];

			if ((curBankId == 0xFF) && (curPatchId == 0xFF))
				break;

			fileDataOffset += 4; // skip over offset
			instrumentTableCount++;
		}

		if (instrumentTableCount == 0)
			error("MILES-MT32: no instruments in instrument file");

		// Allocate space for instruments
		instrumentTablePtr = new MilesMT32InstrumentEntry[instrumentTableCount];

		// Now actually read all entries
		instrumentPtr = instrumentTablePtr;

		fileDataOffset = 0;
		fileDataLeft = fileSize;
		while (1) {
			curPatchId = fileDataPtr[fileDataOffset++];
			curBankId  = fileDataPtr[fileDataOffset++];

			if ((curBankId == 0xFF) && (curPatchId == 0xFF))
				break;

			instrumentOffset = READ_LE_UINT32(fileDataPtr + fileDataOffset);
			fileDataOffset += 4;

			instrumentPtr->bankId = curBankId;
			instrumentPtr->patchId = curPatchId;

			instrumentDataSize = READ_LE_UINT16(fileDataPtr + instrumentOffset);
			if (instrumentDataSize != (MILES_MT32_PATCHDATA_TOTAL_SIZE + 2))
				error("MILES-MT32: unsupported instrument size");

			instrumentOffset += 2;
			// Copy common parameter data
			memcpy(instrumentPtr->commonParameter, fileDataPtr + instrumentOffset, MILES_MT32_PATCHDATA_COMMONPARAMETER_SIZE);
			instrumentPtr->commonParameter[MILES_MT32_PATCHDATA_COMMONPARAMETER_SIZE] = 0xFF; // Terminator
			instrumentOffset += MILES_MT32_PATCHDATA_COMMONPARAMETER_SIZE;

			// Copy partial parameter data
			for (byte partialNr = 0; partialNr < MILES_MT32_PATCHDATA_PARTIALPARAMETERS_COUNT; partialNr++) {
				memcpy(&instrumentPtr->partialParameters[partialNr], fileDataPtr + instrumentOffset, MILES_MT32_PATCHDATA_PARTIALPARAMETER_SIZE);
				instrumentPtr->partialParameters[partialNr][MILES_MT32_PATCHDATA_PARTIALPARAMETER_SIZE] = 0xFF; // Terminator
				instrumentOffset += MILES_MT32_PATCHDATA_PARTIALPARAMETER_SIZE;
			}

			// Instrument read, next instrument please
			instrumentPtr++;
		}

		// Free instrument file data
		delete[] fileDataPtr;
	}

	return new MidiDriver_Miles_MT32(instrumentTablePtr, instrumentTableCount);
}

} // End of namespace Audio