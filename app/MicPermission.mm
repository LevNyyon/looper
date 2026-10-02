// macOS only hands out microphone audio after the user says yes.
#import <AVFoundation/AVFoundation.h>
#include <functional>

bool microphoneAllowed();
void requestMicrophone (std::function<void (bool)> done);

bool microphoneAllowed()
{
    return [AVCaptureDevice authorizationStatusForMediaType: AVMediaTypeAudio] == AVAuthorizationStatusAuthorized;
}

void requestMicrophone (std::function<void (bool)> done)
{
    [AVCaptureDevice requestAccessForMediaType: AVMediaTypeAudio completionHandler: ^(BOOL granted)
    {
        dispatch_async (dispatch_get_main_queue(), ^{ done (granted); });
    }];
}
