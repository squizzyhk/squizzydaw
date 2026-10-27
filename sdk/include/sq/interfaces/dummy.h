/* Squizzy Studio - interfaces of the dummy service (services/dummy). Owner: M0 skeleton.
   Exists to prove module load/unload/reload and multi-interface queryInterface. */
#ifndef SQ_INTERFACES_DUMMY_H
#define SQ_INTERFACES_DUMMY_H

#include "sq/module.h"

#ifdef __cplusplus
namespace sq {

struct IDummyService : IUnknown {
  SQ_INTERFACE(IDummyService, IUnknown, "{42812941-1F61-4F13-8D50-F7A905B737C7}")
  virtual int32_t SQ_CALL add(int32_t a, int32_t b) = 0;
  /* Copies a NUL-terminated greeting; SQ_E_BUFFER_TOO_SMALL if it does not fit. */
  virtual sq_result SQ_CALL getGreeting(char* buf, uint32_t size) = 0;
  /* Number of times the dummy module has been initialised in this process
     (persisted in settings key Services.dummy.InitCount). */
  virtual int64_t SQ_CALL getInitCount() = 0;
};

struct IDummyCounter : IUnknown {
  SQ_INTERFACE(IDummyCounter, IUnknown, "{02FB210C-9A3D-4855-BA67-5BC3666C6F4F}")
  virtual int32_t SQ_CALL increment() = 0;
  virtual int32_t SQ_CALL value() = 0;
};

namespace dummy {
/* Class implementing IDummyService + IDummyCounter. */
inline constexpr sq_guid kServiceClassId = sq::guid("{9C63D502-ED61-4EE2-8E62-41DB33A4689D}");
/* Class implementing only IDummyCounter. */
inline constexpr sq_guid kCounterClassId = sq::guid("{7D0ED4C5-9598-497B-8DD4-3A955C7583DB}");
}  // namespace dummy

}  // namespace sq
#endif

#endif
