#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>

#include <vector>
#include <set>
#include <map>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/socket.h>

using namespace std;

VAStatus va_status;
int ret_val = 0;

#define CHECK_VASTATUS(va_status,func, ret)                             \
if (va_status != VA_STATUS_SUCCESS) {                                   \
    fprintf(stderr,"%s failed with error code %d (%s),exit\n",func, va_status, vaErrorStr(va_status)); \
    ret_val = ret;                                                      \
    exit(1);                                                         \
}

#define handle_error(msg) do { perror(msg); exit(EXIT_FAILURE); } while(0)

static VADisplay va_dpy = NULL;
static int drm_fd = -1;

#define VA_PROFILE_MAP(P)  {P, #P}

static std::map<VAProfile, const char*> profile_map = {
    VA_PROFILE_MAP(VAProfileNone),
};

VADisplay getVADisplay(void)
{
    const char *drm_device_paths[] = {
        "/dev/dri/renderD128",
        "/dev/dri/card0",
        NULL
    };

    for (int i = 0; drm_device_paths[i]; i++) {
        drm_fd = open(drm_device_paths[i], O_RDWR);
        if (drm_fd < 0)
            continue;

        va_dpy = vaGetDisplayDRM(drm_fd);
        if (va_dpy)
            return va_dpy;

        close(drm_fd);
        drm_fd = -1;
    }

    return NULL;
}

void closeVADisplay()
{
    vaTerminate(va_dpy);
    
    if (drm_fd < 0)
        return;

    close(drm_fd);
    drm_fd = -1;
}

int upload_surface(VASurfaceID surf_id)
{
    VAImage va_img = {};
    void *surf_ptr = nullptr;

    va_status = vaDeriveImage(va_dpy, surf_id, &va_img);
    CHECK_VASTATUS(va_status, "vaDeriveImage", 1);
    //printVAImage(va_img);
    uint16_t w = va_img.width;
    uint16_t h = va_img.height;
    uint32_t pitch = va_img.pitches[0];
    uint32_t uv_offset = va_img.offsets[1];

    va_status = vaMapBuffer(va_dpy, va_img.buf, &surf_ptr);
    CHECK_VASTATUS(va_status, "vaMapBuffer", 1);

    vector<char> src(w*h*3/2, 0);
    char* dst = (char*)surf_ptr;
    FILE* fp = fopen("../../test.nv12", "rb");
    fread(src.data(), w*h*3/2, 1, fp);
    fclose(fp);

    memset(dst, 0, va_img.data_size);
    // Y plane
    for (size_t i = 0; i < h; i++)
        memcpy(dst+i*pitch, src.data()+i*w, w);
    // UV plane
    for (size_t i = 0; i < h/2; i++)
        memcpy(dst+ uv_offset + i*pitch, src.data()+(h+i)*w, w);
    
    vaUnmapBuffer(va_dpy, va_img.buf);
    vaDestroyImage(va_dpy, va_img.image_id);

    return 0;
}

int save_surface(VASurfaceID surf_id)
{
    VAImage va_img = {};
    void *surf_ptr = nullptr;

    va_status = vaDeriveImage(va_dpy, surf_id, &va_img);
    CHECK_VASTATUS(va_status, "vaDeriveImage", 1);
    uint16_t w = va_img.width;
    uint16_t h = va_img.height;
    uint32_t pitch = va_img.pitches[0];
    uint32_t uv_offset = va_img.offsets[1];
    //printVAImage(va_img);
    
    va_status = vaMapBuffer(va_dpy, va_img.buf, &surf_ptr);
    CHECK_VASTATUS(va_status, "vaMapBuffer", 1);

    char* src = (char*)surf_ptr;
    vector<char> dst(w*h*3/2, 0);

    // Y plane
    for (size_t i = 0; i < h; i++)
        memcpy(dst.data()+i*w, src+i*pitch, w);
    // UV plane
    for (size_t i = 0; i < h/2; i++)
        memcpy(dst.data()+(h+i)*w, src+uv_offset+i*pitch, w);

    FILE* fp = fopen("../../test.out.nv12", "wb");
    fwrite(dst.data(), w*h*3/2, 1, fp);
    fclose(fp);

    vaUnmapBuffer(va_dpy, va_img.buf);
    vaDestroyImage(va_dpy, va_img.image_id);

    return 0;
}

static
int * recv_fd(int socket, int n) {
    int *fds = (int *)malloc (n * sizeof(int));
    struct msghdr msg = {0};
    struct cmsghdr *cmsg;
    char buf[CMSG_SPACE(n * sizeof(int))], dup[256];
    memset(buf, 0, sizeof(buf));
    struct iovec io = { .iov_base = &dup, .iov_len = sizeof(dup) };

    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);

    if (recvmsg (socket, &msg, 0) < 0)
        handle_error ("Failed to receive message");

    cmsg = CMSG_FIRSTHDR(&msg);

    memcpy (fds, (int *) CMSG_DATA(cmsg), n * sizeof(int));

    return fds;
}

int fd_is_valid(int fd)
{
    return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

int main(int argc, char *argv[])
{

    ssize_t nbytes;
    char buffer[256];
    int sfd, cfd, *fds;
    struct sockaddr_un addr;

    sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd == -1)
            handle_error ("Failed to create socket");

    if (unlink ("/tmp/fd-pass.socket") == -1 && errno != ENOENT)
            handle_error ("Removing socket file failed");

    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/fd-pass.socket", sizeof(addr.sun_path)-1);

    if (bind(sfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un)) == -1)
            handle_error ("Failed to bind to socket");

    if (listen(sfd, 20) == -1)
            handle_error ("Failed to listen on socket");

    cfd = accept(sfd, NULL, NULL);
    if (cfd == -1)
        handle_error ("Failed to accept incoming connection");

    fds = recv_fd (cfd, 1);
    
    int major_ver, minor_ver;
    
    int fd = fds[0];
    
    if (fd_is_valid(fd))
    {
        printf("fd %d is valid\n", fd);
    }
    else
    {
        printf("fd %d is invalid\n", fd);
        exit(1);
    }

    uint32_t width = 1920;
    uint32_t height = 1080;

    VASurfaceID src_surf = VA_INVALID_ID;
    
    va_dpy = getVADisplay();
    va_status = vaInitialize(va_dpy, &major_ver, &minor_ver);
  
    VASurfaceAttribExternalBuffers buf;
    VASurfaceAttribType type;
    VASurfaceAttrib attrib_list[2];
    VASurfaceID surfId;

    //int alignedWidth = (width + 15)/16*16;
    //int alignedHeight = (height + 15)/16*16;
    int alignedWidth = width;
    int alignedHeight = height;
    int stride = width * 4;

    unsigned long prime_fd = fd;

    //buf.pixel_format = format;
    // hard code it as ARGB
    buf.pixel_format = VA_FOURCC_ABGR;
    buf.width = width;
    buf.height = height;
    buf.data_size = stride * alignedHeight ;
    buf.num_buffers = 1;
    buf.num_planes = 1;
    buf.pitches[0] = stride;
    buf.pitches[1] = 0;
    buf.pitches[2] = 0;
    buf.pitches[3] = 0;
    buf.offsets[0] = 0;
    buf.offsets[1] = 0;
    buf.offsets[2] = 0;
    buf.offsets[3] = 0;
    buf.buffers = &prime_fd;
    buf.flags = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    buf.private_data = nullptr;

    attrib_list[0].type = (VASurfaceAttribType)VASurfaceAttribMemoryType;
    attrib_list[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrib_list[0].value.type = VAGenericValueTypeInteger;
    attrib_list[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;

    attrib_list[1].type = (VASurfaceAttribType)VASurfaceAttribExternalBufferDescriptor;
    attrib_list[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrib_list[1].value.type = VAGenericValueTypePointer;
    attrib_list[1].value.value.p = (void *)&buf;

    VAStatus sts = vaCreateSurfaces(va_dpy, VA_RT_FORMAT_RGB32, alignedWidth, alignedHeight, &surfId, 1, attrib_list, 2);
    if (sts != VA_STATUS_SUCCESS)
    {
        fprintf(stderr, "Error: create surface from prime_fd %d failed, returns %d\n", fd, sts);
        return VA_INVALID_SURFACE;
    }
    else
    {
        fprintf(stderr, "Create Surface %d from prime_fd %d\n", surfId, fd);
        return surfId;
    }

    vaDestroySurfaces(va_dpy, &surfId, 1);

    closeVADisplay();

    printf("done\n");
    return 0;
}