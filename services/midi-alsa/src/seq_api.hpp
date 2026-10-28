// midi-alsa: the part of the ALSA sequencer API this driver uses, bound from libasound.so.2 at
// runtime (dlopen/dlsym; no ALSA development headers needed).
#pragma once

#include <string>

#include "seq_event.hpp"

struct pollfd;

namespace sq::alsaseq {

struct snd_seq_t;
struct snd_seq_client_info_t;
struct snd_seq_port_info_t;

constexpr int kOpenDuplex = 3;  // SND_SEQ_OPEN_OUTPUT | SND_SEQ_OPEN_INPUT
constexpr int kNonblock = 1;
// port capabilities
constexpr unsigned kCapRead = 1u << 0;
constexpr unsigned kCapWrite = 1u << 1;
constexpr unsigned kCapSubsRead = 1u << 5;
constexpr unsigned kCapSubsWrite = 1u << 6;
constexpr unsigned kCapNoExport = 1u << 7;
// port types
constexpr unsigned kTypeMidiGeneric = 1u << 1;
constexpr unsigned kTypeApplication = 1u << 20;
// well-known addresses
constexpr int kClientSystem = 0;
constexpr int kPortSystemAnnounce = 1;

struct SeqApi {
  const char* (*strerror)(int);
  int (*lib_error_set_handler)(void (*)(const char*, int, const char*, int, const char*, ...));
  int (*open)(snd_seq_t** seq, const char* name, int streams, int mode);
  int (*close)(snd_seq_t* seq);
  int (*set_client_name)(snd_seq_t* seq, const char* name);
  int (*client_id)(snd_seq_t* seq);
  int (*create_simple_port)(snd_seq_t* seq, const char* name, unsigned caps, unsigned type);
  int (*delete_simple_port)(snd_seq_t* seq, int port);
  int (*connect_from)(snd_seq_t* seq, int myport, int srcClient, int srcPort);
  int (*disconnect_from)(snd_seq_t* seq, int myport, int srcClient, int srcPort);
  int (*poll_descriptors_count)(snd_seq_t* seq, short events);
  int (*poll_descriptors)(snd_seq_t* seq, pollfd* pfds, unsigned space, short events);
  int (*event_input)(snd_seq_t* seq, SeqEvent** ev);
  int (*event_input_pending)(snd_seq_t* seq, int fetch);
  int (*event_output_direct)(snd_seq_t* seq, SeqEvent* ev);
  int (*client_info_malloc)(snd_seq_client_info_t** p);
  void (*client_info_free)(snd_seq_client_info_t* p);
  void (*client_info_set_client)(snd_seq_client_info_t* p, int client);
  int (*client_info_get_client)(const snd_seq_client_info_t* p);
  const char* (*client_info_get_name)(snd_seq_client_info_t* p);
  int (*query_next_client)(snd_seq_t* seq, snd_seq_client_info_t* p);
  int (*port_info_malloc)(snd_seq_port_info_t** p);
  void (*port_info_free)(snd_seq_port_info_t* p);
  void (*port_info_set_client)(snd_seq_port_info_t* p, int client);
  void (*port_info_set_port)(snd_seq_port_info_t* p, int port);
  int (*port_info_get_port)(const snd_seq_port_info_t* p);
  unsigned (*port_info_get_capability)(const snd_seq_port_info_t* p);
  unsigned (*port_info_get_type)(const snd_seq_port_info_t* p);
  const char* (*port_info_get_name)(const snd_seq_port_info_t* p);
  int (*query_next_port)(snd_seq_t* seq, snd_seq_port_info_t* p);
};

// Loads once per process; nullptr (with `why`) when libasound or a symbol is missing.
const SeqApi* loadSeq(std::string* why = nullptr);

}  // namespace sq::alsaseq
