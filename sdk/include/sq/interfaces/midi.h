/* Squizzy Studio - MIDI ports and Standard MIDI Files (plan 2.2, M2/M7).
   OWNER: orchestrator (frozen).
   IMPLEMENTS: services/midi-win (IMidiDriver, category "MidiDriver");
               services/midi-file (IMidiFileReader/IMidiFileWriter, category "MidiFile").
   CONSUMES: engine (recording/playback), formats (import/export), scripting, remote.
   THREADING:
   - IMidiDriver / port open/close: main thread.
   - IMidiInputCallback::onMidi: driver thread with near-RT priority. Treat it like an RT callback:
     copy the events into a lock-free queue and return; no locks, no allocation, no I/O.
   - IMidiOutputPort::send: callable from ANY thread including the audio thread; implementations
     must queue lock-free and never block.
   - MIDI file objects: any single thread; not thread-safe.
   Timestamps are sq_time_monotonic_ns() nanoseconds (same clock as sq_audio_io.host_time_ns). */
#ifndef SQ_INTERFACES_MIDI_H
#define SQ_INTERFACES_MIDI_H

#include "sq/media.h"
#include "sq/module.h"
#include "sq/interfaces/common.h"

/* sq_midi_port_info.flags */
#define SQ_MIDI_PORT_INPUT   0x01u
#define SQ_MIDI_PORT_OUTPUT  0x02u
#define SQ_MIDI_PORT_VIRTUAL 0x04u

/* sq_midi_file_event.status values besides channel messages */
#define SQ_MIDI_STATUS_SYSEX       0xF0u
#define SQ_MIDI_STATUS_SYSEX_ESC   0xF7u
#define SQ_MIDI_STATUS_META        0xFFu

typedef struct sq_midi_port_info {
  uint32_t struct_size;
  char id[256];        /* stable id, persisted in Devices.Midi.* */
  char name[128];
  uint32_t flags;      /* SQ_MIDI_PORT_* */
} sq_midi_port_info;

/* Timestamped live MIDI. Short messages in bytes[0..size); sysex in data/size (bytes unused). */
typedef struct sq_midi_timed_event {
  uint64_t time_ns;    /* input: arrival time; output: due time (0 = immediately) */
  uint8_t bytes[4];
  uint32_t size;
  const uint8_t* data; /* sysex payload valid during the call only, or NULL */
} sq_midi_timed_event;

typedef struct sq_midi_file_info {
  uint32_t struct_size;
  uint32_t format;          /* 0 or 1 (2 is read as 1) */
  uint32_t track_count;
  uint32_t ticks_per_quarter; /* 0 when SMPTE timing is used */
  int32_t smpte_fps;        /* 24/25/29/30 for SMPTE division, else 0 */
  uint32_t ticks_per_frame; /* SMPTE sub-frame resolution, else 0 */
} sq_midi_file_info;

/* One event of an SMF track. Channel messages: status 0x80..0xEF + data1/data2.
   Meta: status 0xFF, meta_type set, payload in data/size. Sysex: status 0xF0/0xF7, payload in
   data/size (without the leading F0). Pointers valid until the reader is released. */
typedef struct sq_midi_file_event {
  uint32_t struct_size;
  uint64_t tick;            /* absolute tick from track start */
  uint8_t status;
  uint8_t data1;
  uint8_t data2;
  uint8_t meta_type;
  uint32_t size;
  const uint8_t* data;
} sq_midi_file_event;

#ifdef __cplusplus
namespace sq {

/* Implemented by the consumer. See THREADING above. `port` is borrowed. */
struct IMidiInputCallback : IUnknown {
  SQ_INTERFACE(IMidiInputCallback, IUnknown, "{58FABF2A-F489-4EA1-BD34-8D4DE8C9CC24}")
  virtual void SQ_CALL onMidi(const sq_midi_port_info* port, const sq_midi_timed_event* events, uint32_t count) = 0;
};

struct IMidiDeviceListener : IUnknown {
  SQ_INTERFACE(IMidiDeviceListener, IUnknown, "{44577B9C-E98C-4A8C-A603-C3474D0DD612}")
  virtual void SQ_CALL onPortsChanged() = 0; /* driver thread; post to main */
};

/* Released = closed. */
struct IMidiInputPort : IUnknown {
  SQ_INTERFACE(IMidiInputPort, IUnknown, "{E850D055-C553-4DF9-9FA3-71731C18943F}")
  virtual sq_result SQ_CALL getInfo(sq_midi_port_info* out) = 0;
  virtual sq_result SQ_CALL start() = 0; /* begins delivering to the callback */
  virtual sq_result SQ_CALL stop() = 0;  /* returns after the last onMidi() finished */
};

struct IMidiOutputPort : IUnknown {
  SQ_INTERFACE(IMidiOutputPort, IUnknown, "{A36DE8B7-143C-47D4-B3DC-8A38A451C234}")
  virtual sq_result SQ_CALL getInfo(sq_midi_port_info* out) = 0;
  /* RT-safe. Events must be sorted by time_ns. SQ_E_BUSY if the queue is full (events dropped). */
  virtual sq_result SQ_CALL send(const sq_midi_timed_event* events, uint32_t count) = 0;
};

struct IMidiDriver : IUnknown {
  SQ_INTERFACE(IMidiDriver, IUnknown, "{FEB37006-B369-44BB-9B2E-EE894A431932}")
  virtual sq_result SQ_CALL refreshPorts() = 0;
  virtual uint32_t SQ_CALL getPortCount() = 0;
  virtual sq_result SQ_CALL getPortInfo(uint32_t index, sq_midi_port_info* out) = 0;
  /* callback add-ref'd until the port is released. */
  virtual sq_result SQ_CALL openInput(const char* portId, IMidiInputCallback* callback, IMidiInputPort** out) = 0;
  virtual sq_result SQ_CALL openOutput(const char* portId, IMidiOutputPort** out) = 0;
  virtual sq_result SQ_CALL addListener(IMidiDeviceListener* listener) = 0;
  virtual sq_result SQ_CALL removeListener(IMidiDeviceListener* listener) = 0;
};

/* SMF 0/1 reader: whole file parsed on open. */
struct IMidiFileReader : IUnknown {
  SQ_INTERFACE(IMidiFileReader, IUnknown, "{E957FC7C-0494-46E1-9654-E4ABE5A357FD}")
  virtual sq_result SQ_CALL open(const char* path) = 0;
  virtual sq_result SQ_CALL openStream(IByteStream* stream) = 0; /* read fully during the call */
  virtual sq_result SQ_CALL getInfo(sq_midi_file_info* out) = 0;
  virtual uint32_t SQ_CALL getEventCount(uint32_t track) = 0;
  virtual sq_result SQ_CALL getEvent(uint32_t track, uint32_t index, sq_midi_file_event* out) = 0;
  /* Track name meta (0x03) or "" */
  virtual sq_result SQ_CALL getTrackName(uint32_t track, char* buf, uint32_t bufSize, uint32_t* needed) = 0;
};

/* SMF writer. Sequence: create -> (beginTrack -> addEvent* -> endTrack)* -> finish.
   Events of a track must have non-decreasing ticks; End-of-Track is added by endTrack(). */
struct IMidiFileWriter : IUnknown {
  SQ_INTERFACE(IMidiFileWriter, IUnknown, "{D8326945-6E77-4845-9A4B-44984C10996D}")
  virtual sq_result SQ_CALL create(const char* path, uint32_t format, uint32_t ticksPerQuarter) = 0;
  virtual sq_result SQ_CALL createStream(IByteStream* stream, uint32_t format, uint32_t ticksPerQuarter) = 0;
  virtual sq_result SQ_CALL beginTrack() = 0;
  virtual sq_result SQ_CALL addEvent(const sq_midi_file_event* event) = 0;
  virtual sq_result SQ_CALL endTrack() = 0;
  virtual sq_result SQ_CALL finish() = 0; /* releasing without finish() discards the output */
};

namespace midi {
inline constexpr const char* kDriverCategory = "MidiDriver";
inline constexpr const char* kFileCategory = "MidiFile";
inline constexpr sq_guid kWinMidiDriverClassId = sq::guid("{2D6E8AD7-C6D5-4415-950C-7F536247163B}");
inline constexpr sq_guid kMidiFileReaderClassId = sq::guid("{3BEC851A-1C32-42C5-BA6E-B29F1509AEB4}");
inline constexpr sq_guid kMidiFileWriterClassId = sq::guid("{1BD6B6C5-4B9E-421B-9BAB-A82D3FEDA0C1}");
}  // namespace midi

}  // namespace sq
#endif /* __cplusplus */

#endif /* SQ_INTERFACES_MIDI_H */
