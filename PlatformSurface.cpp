/*
 * Copyright (c) 2016-2019 WangBin <wbsecg1 at gmail.com>
 * This file is part of UGS (Universal Graphics Surface)
 * Source code: https://github.com/wang-bin/ugs
 * 
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "ugs/PlatformSurface.h"
#include "base/mpsc_fifo.h"
#ifdef WINAPI_FAMILY
# include <winapifamily.h>
# if !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#   define UGS_OS_WINRT
# endif
#endif //WINAPI_FAMILY

UGS_NS_BEGIN
extern PlatformSurface* create_android_surface();
extern PlatformSurface* create_uikit_surface();
extern PlatformSurface* create_wfc(void*);
extern PlatformSurface* create_rpi_surface(void*);
extern PlatformSurface* create_x11_surface(void*);
extern PlatformSurface* create_win32_surface(void*);
extern PlatformSurface* create_winrt_surface();
extern PlatformSurface* create_wayland_surface(void*);
extern PlatformSurface* create_gbm_surface(void*);
extern PlatformSurface* create_malifb_surface(void*);
typedef PlatformSurface* (*surface_creator)(void*);

// TODO: print what is creating
PlatformSurface* PlatformSurface::create(void* handle, Type type)
{
    // android, ios and winrt surface does not create native handle internally, so do not check nativeHandle()
#ifdef __ANDROID__
    return create_android_surface();
#elif defined(__APPLE__)
# if !defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__)
    return create_uikit_surface();
# endif
#elif defined(UGS_OS_WINRT)
    return create_winrt_surface();
#endif
    // for implemented surfaces which are not dummy(wrapper) of native surfaces, return the dummy wrapper surface
    if (handle
#ifdef _WIN32 // Win32Surface supports foreign window, others(gbm, wl, x11, coca) are not implemented
     && type != Type::Default
#endif
     )
        return new PlatformSurface(type); 
#ifdef HAVE_WAYLAND
    if (type == Type::Wayland)
        return create_wayland_surface(handle);
#endif
#ifdef HAVE_X11
    if (type == Type::X11)
        return create_x11_surface(handle);
#endif
#ifdef HAVE_GBM
    if (type == Type::GBM)
        return create_gbm_surface(handle);
#endif
    // fallback to platform default surface
    // TODO: set order by user
    for (auto create_win : std::initializer_list<surface_creator>{ // vs2013 does not support {}, it requires explicit std::initializer_list<surface_creator>{}
#if defined(_WIN32) && !defined(UGS_OS_WINRT)
        create_win32_surface,
#elif defined(OS_RPI)
        //create_wfc,
        create_rpi_surface,
#endif
#if defined(__arm__) && defined(__linux__)
        create_malifb_surface, // accelerated fbdev is preferred over x11
#endif // defined(__arm__) && defined(__linux__)
// APPLE, Cygwin can support X11, but that's not the platform default, i.e. Type::Default
#if (HAVE_X11+0) && defined(__gnu_linux__) && !defined(ANDROID)
        create_x11_surface,
#endif
#if (HAVE_WAYLAND+0)
        create_wayland_surface,
#endif
#if (HAVE_GBM+0)
        create_gbm_surface,
#endif
    }) { // TODO: how to avoid crash if create error?
        PlatformSurface* pw = create_win(handle);
        if (pw && (pw->nativeHandle() || handle))
            return pw;
        delete pw;
    }
    return new PlatformSurface(type);
}

class PlatformSurface::Private
{
public:
    bool closed = false;
    PlatformSurface::Type type = PlatformSurface::Type::Default;
    void* native_handle = nullptr;
    std::function<void(void*)> handle_cb = nullptr;
    std::function<void()> cb = nullptr;
    mpsc_fifo<PlatformSurface::Event> events;
};

PlatformSurface::PlatformSurface(Type type)
    : d(new Private())
{
    d->type = type;
}

PlatformSurface::~PlatformSurface()
{
    delete d;
}

PlatformSurface::Type PlatformSurface::type() const
{
    return d->type;
}

void PlatformSurface::resetNativeHandle(void* handle)
{
    if (d->native_handle == handle) // TODO: no check, used to check resize in callback
        return;
    auto old = d->native_handle;
    d->native_handle = handle;
    // NativeHandleEvent must be posted after nativeHandleForGL is valid, before resize
    if (d->handle_cb)
        d->handle_cb(old);
    Event e;
    e.type = Event::NativeHandle;
    e.handle.before = old;
    e.handle.after = handle;
    pushEvent(std::move(e));
    if (!handle)
        return;
    int w = 0, h = 0;
    if (size(&w, &h))
        PlatformSurface::resize(w, h);
}

void PlatformSurface::setNativeHandleChangeCallback(std::function<void(void*)> cb)
{
    d->handle_cb = cb;
}

void* PlatformSurface::nativeHandle() const
{
    return d->native_handle;
}

void PlatformSurface::setEventCallback(std::function<void()> cb)
{
    d->cb = cb;
}

void PlatformSurface::resize(int w, int h)
{
    Event e;
    e.type = Event::Resize;
    e.size = {w, h};
    pushEvent(std::move(e));
}

void PlatformSurface::close()
{
    pushEvent(Event{Event::Close});
    d->closed = true;
}

bool PlatformSurface::popEvent(Event &e)
{
    // SPSC is enough if events are generated in consumer thread(Produce/Consume is ordered) and at most 1 another thread
    // but processEvents() is also called in RenderLoop.waitForStopped(), so MPSC is require
    processEvents();
    return d->events.pop(&e);
}

void PlatformSurface::pushEvent(Event&& e)
{
    if (d->closed) // no pendding events for closed surface
        return;
    d->events.push(std::move(e));
    // TODO: user listeners
    if (d->cb)
        d->cb();
}
UGS_NS_END
