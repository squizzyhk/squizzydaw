// pluginhost-vst3: the one translation unit that defines the static `iid` members of every
// pluginterfaces interface used by the adapter (the vendored subset ships no iid .cpp).
#define INIT_CLASS_IID
#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivstattributes.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstpluginterfacesupport.h>
