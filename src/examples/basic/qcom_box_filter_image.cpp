//--------------------------------------------------------------------------------------
// File: qcom_box_filter_imagef_example.cpp
// Desc:
//
// Author:      QUALCOMM
//
//               Copyright (c) 2017 QUALCOMM Technologies, Inc.
//                         All Rights Reserved.
//                      QUALCOMM Proprietary/GTDR
//--------------------------------------------------------------------------------------

// Std includes
#include <cstdlib>
#include <cstring>
#include <iostream>

// Project includes
#include "util/cl_wrapper.h"
#include "util/util.h"

// Library includes
#include <CL/cl.h>
#include <CL/cl_ext_qcom.h>

static const char *HELP_MESSAGE = "\n"
"Usage: qcom_box_filter_imagef_example <source image data file> <output image data file>\n"
"\n"
"Runs a kernel demonstrating qcom_box_filter_imagef by downscaling a specified\n"
"image and writing the result to <output image data file> (see README for format info).\n";

static const float SCALE_FACTOR = 4.f;

static const char *PROGRAM_SOURCE[] = {
"__kernel void downscale_single_plane(__read_only     image2d_t               src_image,\n",
"                                     __write_only    image2d_t               downscaled_image,\n",
"                                                     sampler_t               sampler,\n",
"                                                     qcom_box_size_t         box_size,\n",
"                                                     float2                  unpacked_scale)\n",
"{\n",
"    int     wid_x            = get_global_id(0);\n",
"    int     wid_y            = get_global_id(1);\n",
"    int2    downscaled_coord = (int2)(wid_x, wid_y);\n",
"    float2  src_coord        = (float2)((float)wid_x + 0.5f, (float)wid_y + 0.5f) * unpacked_scale;\n",
"    float4  downscaled_pixel = qcom_box_filter_imagef(src_image, sampler, src_coord, box_size);\n",    //高通内置函数qcom_box_filter_imagef
"    write_imagef(downscaled_image, downscaled_coord, downscaled_pixel);\n",
"}\n"
};

static const cl_uint PROGRAM_SOURCE_LEN = sizeof(PROGRAM_SOURCE) / sizeof(const char *);

/*
1:对于nv21的图, 每次都是创建一个image_channel_order = CL_QCOM_NV12;的大cl_mem = clcreateImage对象, 然后基于
    此cl_mem分别clCreateImage Y和UV cl_mem对象, 然后再分别对两组平面Y和UV进行kernel计算. 值得注意的是, 创建UV cl_mem
    对象的时候, 宽高是完整图片的宽高,而不是实际UV像素的宽高. 而且map指针的时候,也不需要做偏置,这个应该是Qcom的扩展驱动自动进行了处理.

2:在map了指针,使用memcpy分别对Y和UV cl_mem对象拷贝数据的时候,使用了map返回的row_pitch参数. 然后再一行一行的拷贝数据. 这也许意味着,
    1:高通的扩展对NV12的cl_mem对象的内存做了一些额外的内存对齐;2:自己以后在写此类代码的时候也需要考虑这点,即可能的内存对齐

3:可以基于同一个program对象上创建两个同名的kernel

4:本例中对于src和out,创建大的NV12 cl_mem的时候,都使用了分配的NV12 ION内存. 但是即使对于src, 也没有直接把数据memcpy到ION内存了, 而
    是等到分别对子Y和UV对象分别map得到指针之后才做的memcpy

*/
int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "Please specify source and output images.\n";
        std::cerr << HELP_MESSAGE;
        std::exit(EXIT_SUCCESS);
    }
    const std::string src_image_filename(argv[1]);
    const std::string out_image_filename(argv[2]);

    cl_wrapper wrapper;
    cl_program   program             = wrapper.make_program(PROGRAM_SOURCE, PROGRAM_SOURCE_LEN);
    cl_kernel    y_plane_kernel      = wrapper.make_kernel("downscale_single_plane", program);  //基于同一个program可以创建多个相同的kernel对象
    cl_kernel    uv_plane_kernel     = wrapper.make_kernel("downscale_single_plane", program);
    cl_context   context             = wrapper.get_context();
    nv12_image_t src_nv12_image_info = load_nv12_image_data(src_image_filename);

    /*
     * Step 0: Confirm the required OpenCL extensions are supported.
     */

    if (!wrapper.check_extension_support("cl_qcom_other_image"))
    {
        std::cerr << "Extension cl_qcom_other_image needed for NV12 image format is not supported.\n";
        std::exit(EXIT_FAILURE);
    }

    if (!wrapper.check_extension_support("cl_qcom_accelerated_image_ops"))
    {
        std::cerr << "Extension cl_qcom_accelerated_image_ops needed for qcom_box_filter_imagef is not supported.\n";
        std::exit(EXIT_FAILURE);
    }

    if (!wrapper.check_extension_support("cl_qcom_ext_host_ptr"))
    {
        std::cerr << "Extension cl_qcom_ext_host_ptr needed for ION-backed images is not supported.\n";
        std::exit(EXIT_FAILURE);
    }

    if (!wrapper.check_extension_support("cl_qcom_ion_host_ptr"))
    {
        std::cerr << "Extension cl_qcom_ion_host_ptr needed for ION-backed images is not supported.\n";
        std::exit(EXIT_FAILURE);
    }

    /*
     * Step 1: Create suitable ion buffer-backed CL images. Note that planar formats (like NV12) must be read only,
     * but you can write to child images derived from the planes. (See step 2 for deriving child images.)
     */

    cl_image_format src_nv12_format;
    src_nv12_format.image_channel_order     = CL_QCOM_NV12;
    src_nv12_format.image_channel_data_type = CL_UNORM_INT8;

    cl_image_desc src_nv12_desc;
    std::memset(&src_nv12_desc, 0, sizeof(src_nv12_desc));
    src_nv12_desc.image_type   = CL_MEM_OBJECT_IMAGE2D;
    src_nv12_desc.image_width  = src_nv12_image_info.y_width;
    src_nv12_desc.image_height = src_nv12_image_info.y_height;

    cl_mem_ion_host_ptr src_nv12_ion_mem = wrapper.make_ion_buffer_for_yuv_image(src_nv12_format, src_nv12_desc);
    cl_int err;
    cl_mem src_nv12_image = clCreateImage(
            context,
            CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR | CL_MEM_EXT_HOST_PTR_QCOM,
            &src_nv12_format,
            &src_nv12_desc,
            &src_nv12_ion_mem,
            &err
    ); 
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " with clCreateImage for source image." << "\n";
        std::exit(err);
    }

    cl_image_format out_nv12_format;
    out_nv12_format.image_channel_order     = CL_QCOM_NV12;
    out_nv12_format.image_channel_data_type = CL_UNORM_INT8;

    cl_image_desc out_nv12_desc;
    std::memset(&out_nv12_desc, 0, sizeof(out_nv12_desc));
    out_nv12_desc.image_type   = CL_MEM_OBJECT_IMAGE2D;
    out_nv12_desc.image_width  = src_nv12_image_info.y_width / static_cast<size_t>(SCALE_FACTOR);
    out_nv12_desc.image_height = src_nv12_image_info.y_height / static_cast<size_t>(SCALE_FACTOR);

    cl_mem_ion_host_ptr out_nv12_ion_mem = wrapper.make_ion_buffer_for_yuv_image(out_nv12_format, out_nv12_desc);
    cl_mem out_nv12_image = clCreateImage(
            context,
            CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR | CL_MEM_EXT_HOST_PTR_QCOM,
            &out_nv12_format,
            &out_nv12_desc,
            &out_nv12_ion_mem,
            &err
    );
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " with clCreateImage for downscaled image." << "\n";
        std::exit(err);
    }

    /*
     * Step 2: Separate planar NV12 images into their component planes.
     */

    cl_image_format src_y_plane_format;
    src_y_plane_format.image_channel_order     = CL_QCOM_NV12_Y;
    src_y_plane_format.image_channel_data_type = CL_UNORM_INT8;

    cl_image_desc src_y_plane_desc;
    std::memset(&src_y_plane_desc, 0, sizeof(src_y_plane_desc));
    src_y_plane_desc.image_type   = CL_MEM_OBJECT_IMAGE2D;
    src_y_plane_desc.image_width  = src_nv12_image_info.y_width;
    src_y_plane_desc.image_height = src_nv12_image_info.y_height;
    src_y_plane_desc.mem_object   = src_nv12_image;

    cl_mem src_y_plane = clCreateImage(
            context,
            CL_MEM_READ_ONLY,
            &src_y_plane_format,
            &src_y_plane_desc,
            NULL,   //没有指定指针
            &err
    );
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " with clCreateImage for source image y plane." << "\n";
        std::exit(err);
    }

    cl_image_format src_uv_plane_format;
    src_uv_plane_format.image_channel_order     = CL_QCOM_NV12_UV;
    src_uv_plane_format.image_channel_data_type = CL_UNORM_INT8;

    cl_image_desc src_uv_plane_desc;
    std::memset(&src_uv_plane_desc, 0, sizeof(src_uv_plane_desc));
    src_uv_plane_desc.image_type   = CL_MEM_OBJECT_IMAGE2D;
    //NOTICE!!!
    // The image dimensions for the uv-plane derived image must be the same as the parent image, even though the
    // actual dimensions of the uv-plane differ by a factor of 2 in each dimension.
    src_uv_plane_desc.image_width  = src_nv12_image_info.y_width;
    src_uv_plane_desc.image_height = src_nv12_image_info.y_height;
    src_uv_plane_desc.mem_object   = src_nv12_image;

    cl_mem src_uv_plane = clCreateImage(
            context,
            CL_MEM_READ_ONLY,
            &src_uv_plane_format,
            &src_uv_plane_desc,
            NULL,   //并没有指定指针，
            &err
    );
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " with clCreateImage for source image uv plane." << "\n";
        std::exit(err);
    }

    cl_image_format out_y_plane_format;
    out_y_plane_format.image_channel_order     = CL_QCOM_NV12_Y;
    out_y_plane_format.image_channel_data_type = CL_UNORM_INT8;

    cl_image_desc out_y_plane_desc;
    std::memset(&out_y_plane_desc, 0, sizeof(out_y_plane_desc));
    out_y_plane_desc.image_type   = CL_MEM_OBJECT_IMAGE2D;
    out_y_plane_desc.image_width  = out_nv12_desc.image_width;
    out_y_plane_desc.image_height = out_nv12_desc.image_height;
    out_y_plane_desc.mem_object   = out_nv12_image;

    cl_mem out_y_plane = clCreateImage(
            context,
            CL_MEM_WRITE_ONLY,
            &out_y_plane_format,
            &out_y_plane_desc,
            NULL,
            &err
    );
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " with clCreateImage for destination image y plane." << "\n";
        std::exit(err);
    }

    cl_image_format out_uv_plane_format;
    out_uv_plane_format.image_channel_order     = CL_QCOM_NV12_UV;
    out_uv_plane_format.image_channel_data_type = CL_UNORM_INT8;

    cl_image_desc out_uv_plane_desc;
    std::memset(&out_uv_plane_desc, 0, sizeof(out_uv_plane_desc));
    out_uv_plane_desc.image_type   = CL_MEM_OBJECT_IMAGE2D;
    // The image dimensions for the uv-plane derived image must be the same as the parent image, even though the
    // actual dimensions of the uv-plane differ by a factor of 2 in each dimension.
    out_uv_plane_desc.image_width  = out_nv12_desc.image_width;
    out_uv_plane_desc.image_height = out_nv12_desc.image_height;
    out_uv_plane_desc.mem_object   = out_nv12_image;

    cl_mem out_uv_plane = clCreateImage(
            context,
            CL_MEM_WRITE_ONLY,  //虽然后面有通过map来把此对象的内存拷贝到host,但是这里也没用到和host相关的memory flag,可能是和使用了ION内存有关
            &out_uv_plane_format,
            &out_uv_plane_desc,
            NULL,
            &err
    );
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " with clCreateImage for destination image uv plane." << "\n";
        std::exit(err);
    }
    //TODO:
    /*
     * Step 3: Copy data to input image planes. Note that for linear NV12 images you must observe row alignment
     * restrictions. (You may also write to the ion buffer directly if you prefer, however using clEnqueueMapImage for
     * a child planar image will return the correct host pointer for the desired plane.)
     */

    cl_command_queue command_queue  = wrapper.get_command_queue();
    const size_t     origin[]       = {0, 0, 0};
    const size_t     src_y_region[] = {src_y_plane_desc.image_width, src_y_plane_desc.image_height, 1};
    size_t           row_pitch      = 0;
    unsigned char   *image_ptr      = static_cast<unsigned char *>(clEnqueueMapImage(
            command_queue,
            src_y_plane,
            CL_TRUE,
            CL_MAP_WRITE,
            origin,
            src_y_region,
            &row_pitch,
            NULL,
            0,
            NULL,
            NULL,
            &err
    ));
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " mapping source image y-plane buffer for writing." << "\n";
        std::exit(err);
    }

    // Copies image data to the ION buffer from the host
    for (uint32_t i = 0; i < src_y_plane_desc.image_height; ++i)
    {
        std::memcpy(
                image_ptr                          + i * row_pitch,
                src_nv12_image_info.y_plane.data() + i * src_y_plane_desc.image_width,
                src_y_plane_desc.image_width
        );
    }

    err = clEnqueueUnmapMemObject(command_queue, src_y_plane, image_ptr, 0, NULL, NULL);
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " unmapping source image y-plane data buffer." << "\n";
        std::exit(err);
    }

    // Note the discrepancy between the child plane image descriptor and the size required by clEnqueueMapImage.
    const size_t src_uv_region[] = {src_uv_plane_desc.image_width / 2, src_uv_plane_desc.image_height / 2, 1};
    row_pitch                    = 0;
    image_ptr = static_cast<unsigned char *>(clEnqueueMapImage(
            command_queue,
            src_uv_plane,
            CL_TRUE,
            CL_MAP_WRITE,
            origin,
            src_uv_region,
            &row_pitch, //返回的row_pitch在memcpy的时候有用到
            NULL,
            0,
            NULL,
            NULL,
            &err
    ));
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " mapping source image uv-plane buffer for writing." << "\n";
        std::exit(err);
    }

    // Copies image data to the ION buffer from the host
    for (uint32_t i = 0; i < src_uv_plane_desc.image_height / 2; ++i)
    {
        std::memcpy(
                image_ptr                           + i * row_pitch,
                src_nv12_image_info.uv_plane.data() + i * src_uv_plane_desc.image_width,
                src_uv_plane_desc.image_width
        );
    }

    err = clEnqueueUnmapMemObject(command_queue, src_uv_plane, image_ptr, 0, NULL, NULL);
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " unmapping source image uv-plane data buffer." << "\n";
        std::exit(err);
    }

    /*
     * Step 4: Set up other kernel arguments
     */

    cl_sampler sampler = clCreateSampler(
            context,
            CL_FALSE,
            CL_ADDRESS_CLAMP_TO_EDGE,
            CL_FILTER_NEAREST,
            &err
    );
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " with clCreateSampler." << "\n";
        std::exit(err);
    }

    cl_box_filter_size_qcom box_size       = {SCALE_FACTOR, SCALE_FACTOR};
    cl_float2               unpacked_scale = {SCALE_FACTOR, SCALE_FACTOR};

    /*
     * Step 5: Run the kernel separately for y- and uv-planes
     */

    err = clSetKernelArg(y_plane_kernel, 0, sizeof(src_y_plane), &src_y_plane);
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " with clSetKernelArg for argument 0 for y-plane kernel." << "\n";
        std::exit(err);
    }

    err = clSetKernelArg(y_plane_kernel, 1, sizeof(out_y_plane), &out_y_plane);
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " with clSetKernelArg for argument 1 for y-plane kernel." << "\n";
        std::exit(err);
    }

    err = clSetKernelArg(y_plane_kernel, 2, sizeof(sampler), &sampler);
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " with clSetKernelArg for argument 2 for y-plane kernel." << "\n";
        std::exit(err);
    }

    err = clSetKernelArg(y_plane_kernel, 3, sizeof(box_size), &box_size);
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " with clSetKernelArg for argument 3 for y-plane kernel." << "\n";
        std::exit(err);
    }

    err = clSetKernelArg(y_plane_kernel, 4, sizeof(unpacked_scale), &unpacked_scale);
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " with clSetKernelArg for argument 4 for y-plane kernel." << "\n";
        std::exit(err);
    }

    err = clSetKernelArg(uv_plane_kernel, 0, sizeof(src_uv_plane), &src_uv_plane);
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " with clSetKernelArg for argument 0 for uv-plane kernel." << "\n";
        std::exit(err);
    }

    err = clSetKernelArg(uv_plane_kernel, 1, sizeof(out_uv_plane), &out_uv_plane);
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " with clSetKernelArg for argument 1 for uv-plane kernel." << "\n";
        std::exit(err);
    }

    err = clSetKernelArg(uv_plane_kernel, 2, sizeof(sampler), &sampler);
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " with clSetKernelArg for argument 2 for uv-plane kernel." << "\n";
        std::exit(err);
    }

    err = clSetKernelArg(uv_plane_kernel, 3, sizeof(box_size), &box_size);
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " with clSetKernelArg for argument 3 for uv-plane kernel." << "\n";
        std::exit(err);
    }

    err = clSetKernelArg(uv_plane_kernel, 4, sizeof(unpacked_scale), &unpacked_scale);
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " with clSetKernelArg for argument 4 for uv-plane kernel." << "\n";
        std::exit(err);
    }

    //运算时使用的是处理之后out的宽高
    //先执行Y平面的处理, 然后执行UV平面的处理
    const size_t y_plane_work_size[] = {out_y_plane_desc.image_width, out_y_plane_desc.image_height};
    err = clEnqueueNDRangeKernel(
            command_queue,
            y_plane_kernel,
            2,
            NULL,
            y_plane_work_size,
            NULL,
            0,
            NULL,
            NULL
    );
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " with clEnqueueNDRangeKernel for y-plane kernel." << "\n";
        std::exit(err);
    }

    const size_t uv_plane_work_size[] = {out_uv_plane_desc.image_width / 2, out_uv_plane_desc.image_height / 2};
    err = clEnqueueNDRangeKernel(
            command_queue,
            uv_plane_kernel,
            2,
            NULL,
            uv_plane_work_size,
            NULL,
            0,
            NULL,
            NULL
    );
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " with clEnqueueNDRangeKernel for uv-plane kernel." << "\n";
        std::exit(err);
    }

    clFinish(command_queue);

    /*
     * Step 6: Copy the data out of the ion buffer for each plane.
     */

    nv12_image_t out_nv12_image_info;
    out_nv12_image_info.y_width  = out_nv12_desc.image_width;
    out_nv12_image_info.y_height = out_nv12_desc.image_height;
    out_nv12_image_info.y_plane.resize(out_nv12_image_info.y_width * out_nv12_image_info.y_height);
    out_nv12_image_info.uv_plane.resize(out_nv12_image_info.y_width * out_nv12_image_info.y_height / 2);

    const size_t out_y_region[] = {out_y_plane_desc.image_width, out_y_plane_desc.image_height, 1};
    row_pitch                   = 0;
    image_ptr = static_cast<unsigned char *>(clEnqueueMapImage(
            command_queue,
            out_y_plane,
            CL_TRUE,
            CL_MAP_READ,
            origin,
            out_y_region,
            &row_pitch,
            NULL,
            0,
            NULL,
            NULL,
            &err
    ));
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " mapping dest image y-plane buffer for reading." << "\n";
        std::exit(err);
    }

    // Copies image data from the ION buffer to the host
    for (uint32_t i = 0; i < out_y_plane_desc.image_height; ++i)
    {
        std::memcpy(
                out_nv12_image_info.y_plane.data() + i * out_y_plane_desc.image_width,
                image_ptr                          + i * row_pitch,
                out_y_plane_desc.image_width
        );
    }

    err = clEnqueueUnmapMemObject(command_queue, out_y_plane, image_ptr, 0, NULL, NULL);
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " unmapping dest image y-plane buffer." << "\n";
        std::exit(err);
    }

    const size_t out_uv_region[] = {out_uv_plane_desc.image_width / 2, out_uv_plane_desc.image_height / 2, 1};
    row_pitch                    = 0;
    image_ptr = static_cast<unsigned char *>(clEnqueueMapImage(
            command_queue,
            out_uv_plane,
            CL_TRUE,
            CL_MAP_READ,
            origin,
            out_uv_region,
            &row_pitch,
            NULL,
            0,
            NULL,
            NULL,
            &err
    ));
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " mapping dest image uv-plane buffer for reading." << "\n";
        std::exit(err);
    }

    // Copies image data from the ION buffer to the host
    for (uint32_t i = 0; i < out_uv_plane_desc.image_height / 2; ++i)
    {
        std::memcpy(
                out_nv12_image_info.uv_plane.data() + i * out_uv_plane_desc.image_width,
                image_ptr                           + i * row_pitch,
                out_uv_plane_desc.image_width
        );
    }

    err = clEnqueueUnmapMemObject(command_queue, out_uv_plane, image_ptr, 0, NULL, NULL);
    if (err != CL_SUCCESS)
    {
        std::cerr << "Error " << err << " unmapping dest image uv-plane buffer." << "\n";
        std::exit(err);
    }

    clFinish(command_queue);

    save_nv12_image_data(out_image_filename, out_nv12_image_info);

    // Clean up cl resources that aren't automatically handled by cl_wrapper
    clReleaseSampler(sampler);
    clReleaseMemObject(src_uv_plane);
    clReleaseMemObject(src_y_plane);
    clReleaseMemObject(src_nv12_image);
    clReleaseMemObject(out_uv_plane);
    clReleaseMemObject(out_y_plane);
    clReleaseMemObject(out_nv12_image);

    return 0;
}
