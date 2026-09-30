#ifndef __EMSCRIPTEN__

#include "NativeFileDialog.h"

#import <Cocoa/Cocoa.h>

namespace NativeFileDialog {

std::string pickDirectory(const std::string& title) {
  @autoreleasepool {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title                   = [NSString stringWithUTF8String:title.c_str()];
    panel.canChooseFiles          = NO;
    panel.canChooseDirectories    = YES;
    panel.allowsMultipleSelection = NO;
    panel.canCreateDirectories    = NO;

    if ([panel runModal] == NSModalResponseOK) {
      return panel.URL.path.UTF8String;
    }
    return {};
  }
}

std::vector<std::string> pickFiles(
    const std::string& title,
    const std::vector<std::string>& extensions)
{
  @autoreleasepool {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title                   = [NSString stringWithUTF8String:title.c_str()];
    panel.canChooseFiles          = YES;
    panel.canChooseDirectories    = NO;
    panel.allowsMultipleSelection = YES;

    if (!extensions.empty()) {
      NSMutableArray<NSString*>* types = [NSMutableArray array];
      for (const auto& ext : extensions)
        [types addObject:[NSString stringWithUTF8String:ext.c_str()]];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      panel.allowedFileTypes = types; // allowedContentTypes needs macOS 12+
#pragma clang diagnostic pop
    }

    std::vector<std::string> result;
    if ([panel runModal] == NSModalResponseOK) {
      for (NSURL* url in panel.URLs)
        result.push_back(url.path.UTF8String);
    }
    return result;
  }
}

std::string saveFile(const std::string& title, const std::string& defaultName,
                     const std::string& extension)
{
  @autoreleasepool {
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.title                = [NSString stringWithUTF8String:title.c_str()];
    panel.nameFieldStringValue = [NSString stringWithUTF8String:defaultName.c_str()];
    panel.canCreateDirectories = YES;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    panel.allowedFileTypes = @[ [NSString stringWithUTF8String:extension.c_str()] ];
#pragma clang diagnostic pop

    if ([panel runModal] == NSModalResponseOK) {
      return panel.URL.path.UTF8String;
    }
    return {};
  }
}

} // namespace NativeFileDialog

#endif // __EMSCRIPTEN__
