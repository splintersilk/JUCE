/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-8-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

#if JUCE_MAC
 #include "../native/juce_CameraDevice_mac.h"
#elif JUCE_WINDOWS
 #include "../native/juce_CameraDevice_windows.h"
#elif JUCE_IOS
 #include "../native/juce_CameraDevice_ios.h"
#elif JUCE_ANDROID
 #include "../native/juce_CameraDevice_android.h"
#endif

#if JUCE_ANDROID || JUCE_IOS
//==============================================================================
class CameraDevice::CameraFactory
{
public:
    static CameraFactory& getInstance()
    {
        static CameraFactory factory;
        return factory;
    }

    void openCamera (int index, OpenCameraResultCallback resultCallback,
                     int minWidth, int minHeight, int maxWidth, int maxHeight, bool useHighQuality)
    {
        auto cameraId = getAvailableDevices()[index];

        // [Splintersilk Patch 0005] A pending entry for this camera means its
        // result was never delivered (open stranded by an app suspend). Treat
        // it as stale: destroy it and open fresh; the old jassertfalse path
        // silently swallowed every reopen in release builds. Destroy-first so
        // the abandoned session stops before the new one starts. Ref doc
        // decisions 2-4.
        {
            const int staleIndex = getCameraIndex (cameraId);

            if (staleIndex != -1)
            {
                JUCE_CAMERA_LOG ("CameraFactory: replacing stale pending open for camera " + cameraId);
                NullCheckedInvocation::invoke (onPendingOpenAnomaly,
                                               PendingOpenAnomaly::stalePendingReplaced, cameraId);
                camerasToOpen.remove (staleIndex);
            }
        }

        std::unique_ptr<CameraDevice> device (new CameraDevice (cameraId, index,
                                                                minWidth, minHeight, maxWidth,
                                                                maxHeight, useHighQuality));

        camerasToOpen.add ({ nextRequestId++,
                             std::unique_ptr<CameraDevice> (device.release()),
                             resultCallback });

        auto& pendingOpen = camerasToOpen.getReference (camerasToOpen.size() - 1);

        // [Splintersilk Patch 0005] Deliver results by requestId: the iOS error
        // paths pass an empty deviceId, so the stock deviceId lookup silently
        // dropped every error result. Ref doc decision 1.
        pendingOpen.device->pimpl->open ([this, requestId = pendingOpen.requestId, cameraId] (const String&, const String& error)
                                         {
                                             // resultCallback may destroy the device, freeing this closure
                                             // mid-call: copy requestId out first; after the callback touch
                                             // only stack locals. Ref doc decision 6.
                                             const int completedRequestId = requestId;

                                             int cIndex = getRequestIndex (completedRequestId);

                                             if (cIndex == -1)
                                             {
                                                 NullCheckedInvocation::invoke (onPendingOpenAnomaly,
                                                                                PendingOpenAnomaly::lateResultDropped, cameraId);
                                                 return;
                                             }

                                             auto& cameraPendingOpen = camerasToOpen.getReference (cIndex);

                                             if (error.isEmpty())
                                                 cameraPendingOpen.resultCallback (cameraPendingOpen.device.release(), error);
                                             else
                                                 cameraPendingOpen.resultCallback (nullptr, error);

                                             MessageManager::callAsync ([this, completedRequestId]() { removeRequestWithId (completedRequestId); });
                                         });
    }

private:
    int getCameraIndex (const String& cameraId) const
    {
        for (int i = 0; i < camerasToOpen.size(); ++i)
        {
            auto& pendingOpen = camerasToOpen.getReference (i);

            if (pendingOpen.device->pimpl->getCameraId() == cameraId)
                return i;
        }

        return -1;
    }

    // [Splintersilk Patch 0005] requestId-keyed mirror of getCameraIndex, for
    // result delivery (see the open lambda above).
    int getRequestIndex (int requestId) const
    {
        for (int i = 0; i < camerasToOpen.size(); ++i)
            if (camerasToOpen.getReference (i).requestId == requestId)
                return i;

        return -1;
    }

    void removeRequestWithId (int id)
    {
        for (int i = camerasToOpen.size(); --i >= 0;)
        {
            if (camerasToOpen.getReference (i).requestId == id)
            {
                camerasToOpen.remove (i);
                return;
            }
        }
    }

    struct PendingCameraOpen
    {
        int requestId;
        std::unique_ptr<CameraDevice> device;
        OpenCameraResultCallback resultCallback;
    };

    Array<PendingCameraOpen> camerasToOpen;
    static int nextRequestId;
};

int CameraDevice::CameraFactory::nextRequestId = 0;

#endif

// [Splintersilk Patch 0005] Defined unconditionally so the symbol exists on
// every platform; only the iOS/Android factory invokes it.
std::function<void (CameraDevice::PendingOpenAnomaly, const String&)> CameraDevice::onPendingOpenAnomaly;

//==============================================================================
CameraDevice::CameraDevice (const String& nm, int index, int minWidth, int minHeight, int maxWidth, int maxHeight, bool useHighQuality)
   : name (nm), pimpl (new Pimpl (*this, name, index, minWidth, minHeight, maxWidth, maxHeight, useHighQuality))
{
}

CameraDevice::~CameraDevice()
{
    jassert (juce::MessageManager::getInstance()->currentThreadHasLockedMessageManager());

    stopRecording();
    pimpl.reset();
}

Component* CameraDevice::createViewerComponent()
{
    return new ViewerComponent (*this);
}

void CameraDevice::takeStillPicture (std::function<void (const Image&)> pictureTakenCallback,
                                     const bool skipReorientation)
{
#if JUCE_IOS
    if (skipReorientation)
        return pimpl->takeStillPictureWithoutReorientation (pictureTakenCallback);
#endif
    
    pimpl->takeStillPicture (pictureTakenCallback);
}

void CameraDevice::startRecordingToFile (const File& file, int quality)
{
    stopRecording();
    pimpl->startRecordingToFile (file, quality);
}

Time CameraDevice::getTimeOfFirstRecordedFrame() const
{
    return pimpl->getTimeOfFirstRecordedFrame();
}

void CameraDevice::stopRecording()
{
    pimpl->stopRecording();
}

void CameraDevice::addListener (Listener* listenerToAdd)
{
    if (listenerToAdd != nullptr)
        pimpl->addListener (listenerToAdd);
}

void CameraDevice::removeListener (Listener* listenerToRemove)
{
    if (listenerToRemove != nullptr)
        pimpl->removeListener (listenerToRemove);
}

//==============================================================================
StringArray CameraDevice::getAvailableDevices()
{
    JUCE_AUTORELEASEPOOL
    {
        return Pimpl::getAvailableDevices();
    }
}

CameraDevice* CameraDevice::openDevice ([[maybe_unused]] int index,
                                        [[maybe_unused]] int minWidth, [[maybe_unused]] int minHeight,
                                        [[maybe_unused]] int maxWidth, [[maybe_unused]] int maxHeight,
                                        [[maybe_unused]] bool useHighQuality)
{
    jassert (juce::MessageManager::getInstance()->currentThreadHasLockedMessageManager());

   #if ! JUCE_ANDROID && ! JUCE_IOS
    std::unique_ptr<CameraDevice> d (new CameraDevice (getAvailableDevices() [index], index,
                                                       minWidth, minHeight, maxWidth, maxHeight, useHighQuality));
    if (d != nullptr && d->pimpl->openedOk())
        return d.release();
   #else
    // Use openDeviceAsync to open a camera device on iOS or Android.
    jassertfalse;
   #endif

    return nullptr;
}

void CameraDevice::openDeviceAsync (int index, OpenCameraResultCallback resultCallback,
                                    int minWidth, int minHeight, int maxWidth, int maxHeight, bool useHighQuality)
{
    jassert (juce::MessageManager::getInstance()->currentThreadHasLockedMessageManager());

    if (resultCallback == nullptr)
    {
        // A valid callback must be passed.
        jassertfalse;
        return;
    }

   #if JUCE_ANDROID || JUCE_IOS
    CameraFactory::getInstance().openCamera (index, std::move (resultCallback),
                                             minWidth, minHeight, maxWidth, maxHeight, useHighQuality);
   #else
    auto* device = openDevice (index, minWidth, minHeight, maxWidth, maxHeight, useHighQuality);

    resultCallback (device, device != nullptr ? String() : "Could not open camera device");
   #endif
}

} // namespace juce
