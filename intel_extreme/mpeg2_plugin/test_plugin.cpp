#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <MediaFile.h>
#include <MediaTrack.h>

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "/boot/home/Desktop/test_mediaplayer.m2v";
    
    printf("Opening %s...\n", path);
    
    entry_ref ref;
    get_ref_for_path(path, &ref);
    
    BMediaFile file(&ref);
    status_t st = file.InitCheck();
    if (st != B_OK) {
        printf("BMediaFile init failed: %s (0x%08x)\n", strerror(st), st);
        return 1;
    }
    
    int32 trackCount = file.CountTracks();
    printf("Tracks: %d\n", trackCount);
    
    for (int32 i = 0; i < trackCount; i++) {
        BMediaTrack* track = file.TrackAt(i);
        if (!track) continue;
        
        media_format fmt;
        st = track->EncodedFormat(&fmt);
        printf("Track %d: type=%c%c%c%c, status=%s\n", i,
            (char)(fmt.type>>24), (char)(fmt.type>>16),
            (char)(fmt.type>>8), (char)fmt.type,
            strerror(st));
        
        if (fmt.type == B_MEDIA_ENCODED_VIDEO) {
            media_format decoded;
            decoded.type = B_MEDIA_RAW_VIDEO;
            decoded.u.raw_video.display.format = B_RGB32;
            
            st = track->DecodedFormat(&decoded);
            printf("  DecodedFormat: %s\n", strerror(st));
            printf("  Video: %ux%u, bpr=%u, cs=0x%x\n",
                decoded.u.raw_video.display.line_width,
                decoded.u.raw_video.display.line_count,
                decoded.u.raw_video.display.bytes_per_row,
                decoded.u.raw_video.display.format);
            
            media_codec_info codecInfo;
            st = track->GetCodecInfo(&codecInfo);
            if (st == B_OK)
                printf("  Codec: %s (%s)\n", codecInfo.pretty_name, codecInfo.short_name);
            
            // Try decoding one frame
            int64 frameCount = track->CountFrames();
            printf("  Frames: %lld\n", frameCount);
            
            uint32 w = decoded.u.raw_video.display.line_width;
            uint32 h = decoded.u.raw_video.display.line_count;
            uint32 bpr = decoded.u.raw_video.display.bytes_per_row;
            size_t bufSize = bpr * h;
            
            void* buffer = malloc(bufSize);
            if (buffer) {
                int64 count = 1;
                media_header hdr;
                st = track->ReadFrames(buffer, &count, &hdr);
                printf("  ReadFrames: %s (count=%lld, size=%u)\n",
                    strerror(st), count, hdr.size_used);
                
                if (st == B_OK) {
                    uint8* p = (uint8*)buffer;
                    printf("  First pixels: %u %u %u %u\n",
                        p[0], p[1], p[2], p[3]);
                }
                free(buffer);
            }
        }
        
        file.ReleaseTrack(track);
    }
    
    return 0;
}
