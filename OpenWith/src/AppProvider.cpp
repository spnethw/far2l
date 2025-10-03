#include "AppProvider.hpp"

#ifdef ENABLE_GIO_SUPPORT
#include "GIOBasedAppProvider.hpp"
#endif

#include "XDGBasedAppProvider.hpp"
#include "MacOSAppProvider.hpp"
#include "DummyAppProvider.hpp"

std::unique_ptr<AppProvider> AppProvider::CreateAppProvider(TMsgGetter msg_getter)
{
	std::unique_ptr<AppProvider> provider;

#ifdef ENABLE_GIO_SUPPORT
	provider = std::make_unique<GIOBasedAppProvider>(msg_getter);
#else
    #if defined (__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
        provider = std::make_unique<XDGBasedAppProvider>(msg_getter);
    #elif defined(__APPLE__)
        provider = std::make_unique<MacOSAppProvider>(msg_getter);
    #else
        provider = std::make_unique<DummyAppProvider>(msg_getter);
    #endif
#endif

	if (provider) {
		provider->LoadPlatformSettings();
	}

	return provider;
}
