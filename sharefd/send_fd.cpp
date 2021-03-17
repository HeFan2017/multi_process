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

void send_fd(int socket, int *fds, int n)  // send fd by socket
{
    struct msghdr msg = {0};
    struct cmsghdr *cmsg;
    char buf[CMSG_SPACE(n * sizeof(int))], dup[256];
    memset(buf, 0, sizeof(buf));
    struct iovec io = { .iov_base = &dup, .iov_len = sizeof(dup) };

    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);

    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(n * sizeof(int));

    memcpy ((int *) CMSG_DATA(cmsg), fds, n * sizeof (int));

    if (sendmsg (socket, &msg, 0) < 0)
    {
        printf ("Failed to send message\n");
        exit(1);
    }
}

int accept()
{
   
    int major_ver, minor_ver;
    
    int fd = 4;

    uint32_t width = 1920;
    uint32_t height = 1080;

 
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

int main()
{
    int major_ver, minor_ver;

    uint32_t srcw = 1920;
    uint32_t srch = 1080;
    uint32_t src_fourcc  = VA_FOURCC_ABGR;
    uint32_t src_format  = VA_RT_FORMAT_YUV420;
    VASurfaceID src_surf = VA_INVALID_ID;
    
    va_dpy = getVADisplay();
    va_status = vaInitialize(va_dpy, &major_ver, &minor_ver);
  
    VASurfaceAttrib surf_attrib = {};
    surf_attrib.type =  VASurfaceAttribPixelFormat;
    surf_attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
    surf_attrib.value.type = VAGenericValueTypeInteger;
    surf_attrib.value.value.i = src_fourcc;
    va_status = vaCreateSurfaces(va_dpy, src_format, srcw, srch, &src_surf, 1, &surf_attrib, 1);
    CHECK_VASTATUS(va_status, "vaCreateSurfaces", 1);
    printf("####LOG: src_surf = %d\n", src_surf);
    
    
    // extract the FD from surface
    VADRMPRIMESurfaceDescriptor desc = {};
    va_status = vaExportSurfaceHandle(va_dpy, src_surf, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, &desc);
    CHECK_VASTATUS(va_status, "vaExportSurfaceHandle", 1);
    
    printf("Surface width %d, height %d, fd %d\n", desc.width, desc.height, desc.objects[0].fd);
    
    //execl("./AcceptFD", "./AcceptFD", "4", NULL); // not work
    //if (fork() == 0)
    //{
    //    accept(); // work
    //}
    //else{
    //    sleep(5);
    //}
    
    int sfd, fds[2];
    struct sockaddr_un addr;

    sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd == -1)
    {
        printf ("Failed to create socket\n");
        exit(1);
    }

    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/fd-pass.socket", sizeof(addr.sun_path)-1);

    if (connect(sfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un)) == -1)
    {
        printf ("Failed to connect to socket\n");
        exit(1);
    }

    fds[0] = desc.objects[0].fd;
    send_fd (sfd, fds, 1);

    vaDestroySurfaces(va_dpy, &src_surf, 1);

    closeVADisplay();

    printf("done\n");
    return 0;
}