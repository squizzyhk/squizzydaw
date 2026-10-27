/* Squizzy Studio SDK - base interface: refcounted, vtable-dispatched (COM-style, no COM dependency).

   BINARY CONTRACT (applies to every interface in sq/ and sq/interfaces/):
   - An interface pointer points to an object whose first member is a pointer to a table of
     function pointers ("vtable"). Entries appear in declaration order, base interface first.
   - Every method uses the SQ_CALL convention and takes the interface pointer as hidden first
     argument ("self"). C++ declares interfaces as structs of pure virtual functions with single
     inheritance only; MSVC, clang and GCC lay these out exactly as the C vtable structs below.
   - No virtual destructors in interfaces, no overloads, no data members, no STL/C++ types in
     signatures: only fixed-width ints, float/double, const char* (UTF-8), sq_guid, POD structs
     with a leading struct_size field, and interface pointers.
   - Never let a C++ exception cross an interface or export boundary.
   - Interfaces are immutable once published: to extend, derive a new interface with a new IID.
   - Refcounting: queryInterface/creation return an add-ref'd pointer; the receiver releases it.
     Methods that return a "borrowed" pointer say so explicitly.
*/
#ifndef SQ_UNKNOWN_H
#define SQ_UNKNOWN_H

#include "sq/guid.h"

/* {70519AAF-D202-4A40-94C8-39AF01486455} */
#define SQ_IID_IUNKNOWN_INIT SQ_GUID_INIT(0x70519AAF, 0xD202, 0x4A40, 0x94, 0xC8, 0x39, 0xAF, 0x01, 0x48, 0x64, 0x55)

#ifdef __cplusplus

/* Declares the IID and base of a C++ interface. Use inside the struct body:
     struct IFoo : sq::IUnknown { SQ_INTERFACE(IFoo, sq::IUnknown, "{...}") virtual ... = 0; }; */
#define SQ_INTERFACE(Self, Base, guidString) \
  using sq_base = Base;                      \
  static constexpr sq_guid iid = ::sq::guid(guidString);

namespace sq {

struct IUnknown {
  using sq_base = void;
  static constexpr sq_guid iid = sq_guid SQ_IID_IUNKNOWN_INIT;

  virtual sq_result SQ_CALL queryInterface(const sq_guid* riid, void** out) = 0;
  virtual uint32_t SQ_CALL addRef() = 0;
  virtual uint32_t SQ_CALL release() = 0;
};

}  // namespace sq

typedef sq::IUnknown sq_IUnknown;

#else /* C view */

typedef struct sq_IUnknown sq_IUnknown;
typedef struct sq_IUnknownVtbl {
  sq_result(SQ_CALL* queryInterface)(sq_IUnknown* self, const sq_guid* riid, void** out);
  uint32_t(SQ_CALL* addRef)(sq_IUnknown* self);
  uint32_t(SQ_CALL* release)(sq_IUnknown* self);
} sq_IUnknownVtbl;
struct sq_IUnknown {
  const sq_IUnknownVtbl* vtbl;
};

#endif

#endif /* SQ_UNKNOWN_H */
