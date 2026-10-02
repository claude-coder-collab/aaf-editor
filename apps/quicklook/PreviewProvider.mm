#import <Foundation/Foundation.h>
#import <QuickLookUI/QuickLookUI.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <os/log.h>

#include <aaf/preview/preview.hpp>

#include <exception>
#include <filesystem>
#include <string>

@interface AAFPreviewProvider : QLPreviewProvider <QLPreviewingController>
@end

@implementation AAFPreviewProvider

- (void)providePreviewForFileRequest:(QLFilePreviewRequest*)request completionHandler:(void (^)(QLPreviewReply* _Nullable, NSError* _Nullable))handler
{
    std::string html;
    try
    {
        html = aaf::preview::previewFile(std::filesystem::path(request.fileURL.fileSystemRepresentation));
    }
    catch (const std::exception& e)
    {
        html = aaf::preview::renderErrorPreview(request.fileURL.lastPathComponent.UTF8String, e.what());
    }
    os_log(OS_LOG_DEFAULT, "AAF preview rendered %zu bytes", html.size());
    NSData* data = [NSData dataWithBytes:html.data() length:html.size()];
    QLPreviewReply* reply = [[QLPreviewReply alloc] initWithDataOfContentType:UTTypeHTML
                                                                  contentSize:CGSizeMake(1000, 720)
                                                            dataCreationBlock:^NSData* _Nullable(QLPreviewReply* _Nonnull, NSError* _Nullable* _Nullable) {
                                                              return data;
                                                            }];
    reply.stringEncoding = NSUTF8StringEncoding;
    handler(reply, nil);
}

@end
