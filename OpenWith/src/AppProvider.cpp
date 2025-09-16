#include "AppProvider.hpp"

#include "LinuxAppProvider.hpp"
#include "MacOSAppProvider.hpp"
#include "BSDAppProvider.hpp"
#include "DummyAppProvider.hpp"

std::unique_ptr<AppProvider> AppProvider::CreateAppProvider()
{
#ifdef __linux__
	return std::make_unique<LinuxAppProvider>();
#elif defined(__APPLE__)
	return std::make_unique<MacOSAppProvider>();
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
	return std::make_unique<BSDAppProvider>();
#else
	return std::make_unique<DummyAppProvider>();
#endif
}
