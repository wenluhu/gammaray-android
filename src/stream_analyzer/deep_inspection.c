#define _GNU_SOURCE
#include "color.h"
#include "deep_inspection.h"
#include "__bson.h" /* TODO: fix BSON library to be more friendly */
#include "bson.h"

#include "zmq.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <unistd.h>

#ifndef HOST_NAME_MAX
    #define HOST_NAME_MAX 256
#endif

#define FILE_DATA_WRITE "data"
#define FILE_META_WRITE "metadata"
#define VM_NAME_MAX 512
#define PATH_MAX 4096

void zmq_send_print_error(int err)
{
    switch(err)
    {
        case EAGAIN:
            fprintf_light_red(stderr, "Non-blocking mode was requested and the"
                                      " message cannot be sent at the moment."
                                      "\n");
            break;
        
        case ENOTSUP:
            fprintf_light_red(stderr, "The zmq_send() operation is not "
                                      "supported by this socket type.\n");
            break;

        case EFSM:
            fprintf_light_red(stderr, "The zmq_send() operation cannot be "
                                      "performed on this socket at the moment "
                                      "due to the socket not being in the "
                                      "appropriate state. This error may "
                                      "occur with socket types that switch "
                                      "between several states, such as "
                                      "ZMQ_REP. See the messaging patterns "
                                      "section of zmq_socket(3) for more "
                                      "information.\n");
            break;
        
        case ETERM:
            fprintf_light_red(stderr, "The 0MQ context associated with the "
                                      "specified socket was terminated.\n");
            break;
        
        case ENOTSOCK:
            fprintf_light_red(stderr,  "The provided socket was invalid.\n");
            break;
        
        case EINTR:
            fprintf_light_red(stderr, "The operation was interrupted by "
                                      "delivery of a signal before the "
                                      "message was sent.\n");
            break;
        
        case EFAULT:
            fprintf_light_red(stderr, "Invalid message.\n");
            break;
    }
}

char* construct_channel_name(char* vmname, char* path)
{
    char* buf = malloc(HOST_NAME_MAX + VM_NAME_MAX + PATH_MAX + 3);
    if (buf == NULL)
        return NULL;

    if (gethostname(buf, HOST_NAME_MAX))
    {
        free(buf);
        return NULL;
    }

    strncat(buf, ":", 1);
    strncat(buf, vmname, strlen(vmname));
    strncat(buf, ":", 1);
    strncat(buf, path, strlen(path));
    return buf;    
}

void qemu_free(void* data, void* hint)
{
    free(data);
}

char* clone_cstring(char* cstring)
{
    char* ret = malloc(strlen(cstring) + 1);
    if (ret == NULL)
        return NULL;
    strcpy(ret, cstring);
    return ret;
}

void qemu_parse_header(uint8_t* event_stream, struct qemu_bdrv_write* write)
{
    write->header = *((struct qemu_bdrv_write_header*) event_stream);
}

int qemu_print_sector_type(enum SECTOR_TYPE type)
{
    switch(type)
    {
        case SECTOR_MBR:
            fprintf_light_green(stdout, "Write to MBR detected.\n");
            return 0;
        case SECTOR_EXT2_SUPERBLOCK:
            fprintf_light_green(stdout, "Write to ext2 superblock detected.\n");
            return 0;
        case SECTOR_EXT2_BLOCK_GROUP_DESCRIPTOR:
            fprintf_light_green(stdout, "Write to ext2 block group descriptor detected.\n");
            return 0;
        case SECTOR_EXT2_BLOCK_GROUP_BLOCKMAP:
            fprintf_light_green(stdout, "Write to ext2 block group block map detected.\n");
            return 0;
        case SECTOR_EXT2_BLOCK_GROUP_INODEMAP:
            fprintf_light_green(stdout, "Write to ext2 block group inode map detected.\n");
            return 0;
        case SECTOR_EXT2_INODE:
            fprintf_light_green(stdout, "Write to ext2 inode detected.\n");
            return 0;
        case SECTOR_EXT2_DATA:
            fprintf_light_green(stdout, "Write to ext2 data block detected.\n");
            return 0;
        case SECTOR_EXT2_PARTITION:
            fprintf_light_green(stdout, "Write to ext2 partition detected.\n");
            return 0;
        case SECTOR_UNKNOWN:
            fprintf_light_red(stdout, "Unknown sector type.\n");
    }

    return -1;
}

int ext2_compare_inodes(struct ext2_inode* old_inode,
                        struct ext2_inode* new_inode, void* pub_socket,
                        char* vmname, char* path)
{
    uint64_t i;
    char* channel_name;
    zmq_msg_t msg;
    struct bson_info* bson;
    struct bson_kv val;
    uint8_t* buf;
    uint64_t old, new;

    bson = bson_init();

    if (old_inode->i_mode != new_inode->i_mode)
    {
        fprintf_yellow(stdout, "inode mode modified.\n");

        val.type = BSON_STRING;
        val.size = strlen("inode.i_mode");
        val.key = "type";
        val.data = "inode.i_mode";

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "old";
        old = old_inode->i_mode;
        val.data = &(old);

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "new";
        new = new_inode->i_mode;
        val.data = &(new);

        bson_serialize(bson, &val);
        bson_finalize(bson);
        
        channel_name = construct_channel_name(vmname, path);
        buf = malloc(bson->size + strlen(channel_name) + 1);
        memcpy((uint8_t*) mempcpy(buf, channel_name, strlen(channel_name) + 1),
                                bson->buffer, bson->size);

        if (zmq_msg_init_data(&msg, buf, bson->size +
                              strlen(channel_name) + 1, 
                              qemu_free, 0))
        {
            fprintf_light_red(stderr, "Failure initializing "
                                      "zmq message data.\n");
            return -1;
        }

        free(channel_name);
        bson_reset(bson);

        if (zmq_send(pub_socket, &msg, 0))
        {
            fprintf_light_red(stderr, "Failure sending zmq "
                                      "message.\n");
            zmq_send_print_error(errno);
            return -1;
        }

        if (zmq_msg_close(&msg))
        {
            fprintf_light_red(stderr, "Failure closing zmq "
                                      "message.\n");
            return -1;
        }
    }

    if (old_inode->i_uid != new_inode->i_uid)
    {
        fprintf_yellow(stdout, "owner modified.\n");
    
        val.type = BSON_STRING;
        val.size = strlen("inode.i_uid");
        val.key = "type";
        val.data = "inode.i_uid";

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "old";
        old = old_inode->i_uid;
        val.data = &(old);

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "new";
        new = new_inode->i_uid;
        val.data = &(new);

        bson_serialize(bson, &val);
        bson_finalize(bson);
        
        channel_name = construct_channel_name(vmname, path);
        buf = malloc(bson->size + strlen(channel_name) + 1);
        memcpy((uint8_t*) mempcpy(buf, channel_name, strlen(channel_name) + 1),
                                bson->buffer, bson->size);

        if (zmq_msg_init_data(&msg, buf, bson->size +
                              strlen(channel_name) + 1, 
                              qemu_free, 0))
        {
            fprintf_light_red(stderr, "Failure initializing "
                                      "zmq message data.\n");
            return -1;
        }

        free(channel_name);
        bson_reset(bson);

        if (zmq_send(pub_socket, &msg, 0))
        {
            fprintf_light_red(stderr, "Failure sending zmq "
                                      "message.\n");
            zmq_send_print_error(errno);
            return -1;
        }

        if (zmq_msg_close(&msg))
        {
            fprintf_light_red(stderr, "Failure closing zmq "
                                      "message.\n");
            return -1;
        }
    }

    if (old_inode->i_size != new_inode->i_size)
    {
        fprintf_light_yellow(stdout, "inode size modified, old=%"PRIu32" new=%"PRIu32".\n",
                                      old_inode->i_size, new_inode->i_size);
    
        val.type = BSON_STRING;
        val.size = strlen("inode.i_size");
        val.key = "type";
        val.data = "inode.i_size";

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "old";
        old = old_inode->i_size;
        val.data = &(old);

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "new";
        new = new_inode->i_size;
        val.data = &(new);

        bson_serialize(bson, &val);
        bson_finalize(bson);
        
        channel_name = construct_channel_name(vmname, path);
        buf = malloc(bson->size + strlen(channel_name) + 1);
        memcpy((uint8_t*) mempcpy(buf, channel_name, strlen(channel_name) + 1),
                                bson->buffer, bson->size);

        if (zmq_msg_init_data(&msg, buf, bson->size +
                              strlen(channel_name) + 1, 
                              qemu_free, 0))
        {
            fprintf_light_red(stderr, "Failure initializing "
                                      "zmq message data.\n");
            return -1;
        }

        free(channel_name);
        bson_reset(bson);

        if (zmq_send(pub_socket, &msg, 0))
        {
            fprintf_light_red(stderr, "Failure sending zmq "
                                      "message.\n");
            zmq_send_print_error(errno);
            return -1;
        }

        if (zmq_msg_close(&msg))
        {
            fprintf_light_red(stderr, "Failure closing zmq "
                                      "message.\n");
            return -1;
        }
    }

    if (old_inode->i_atime != new_inode->i_atime)
    {
        fprintf_yellow(stdout, "inode atime modified.\n");
    
        val.type = BSON_STRING;
        val.size = strlen("inode.i_atime");
        val.key = "type";
        val.data = "inode.i_atime";

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "old";
        old = old_inode->i_atime;
        val.data = &(old);

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "new";
        new = new_inode->i_atime;
        val.data = &(new);

        bson_serialize(bson, &val);
        bson_finalize(bson);
        
        channel_name = construct_channel_name(vmname, path);
        buf = malloc(bson->size + strlen(channel_name) + 1);
        memcpy((uint8_t*) mempcpy(buf, channel_name, strlen(channel_name) + 1),
                                bson->buffer, bson->size);

        if (zmq_msg_init_data(&msg, buf, bson->size +
                              strlen(channel_name) + 1, 
                              qemu_free, 0))
        {
            fprintf_light_red(stderr, "Failure initializing "
                                      "zmq message data.\n");
            return -1;
        }

        free(channel_name);
        bson_reset(bson);

        if (zmq_send(pub_socket, &msg, 0))
        {
            fprintf_light_red(stderr, "Failure sending zmq "
                                      "message.\n");
            zmq_send_print_error(errno);
            return -1;
        }

        if (zmq_msg_close(&msg))
        {
            fprintf_light_red(stderr, "Failure closing zmq "
                                      "message.\n");
            return -1;
        }
    }

    if (old_inode->i_ctime != new_inode->i_ctime)
    {
        fprintf_yellow(stdout, "inode ctime modified.\n");
    
        val.type = BSON_STRING;
        val.size = strlen("inode.i_ctime");
        val.key = "type";
        val.data = "inode.i_ctime";

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "old";
        old = old_inode->i_ctime;
        val.data = &(old);

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "new";
        new = new_inode->i_ctime;
        val.data = &(new);

        bson_serialize(bson, &val);
        bson_finalize(bson);
        
        channel_name = construct_channel_name(vmname, path);
        buf = malloc(bson->size + strlen(channel_name) + 1);
        memcpy((uint8_t*) mempcpy(buf, channel_name, strlen(channel_name) + 1),
                                bson->buffer, bson->size);

        if (zmq_msg_init_data(&msg, buf, bson->size +
                              strlen(channel_name) + 1, 
                              qemu_free, 0))
        {
            fprintf_light_red(stderr, "Failure initializing "
                                      "zmq message data.\n");
            return -1;
        }

        free(channel_name);
        bson_reset(bson);

        if (zmq_send(pub_socket, &msg, 0))
        {
            fprintf_light_red(stderr, "Failure sending zmq "
                                      "message.\n");
            zmq_send_print_error(errno);
            return -1;
        }

        if (zmq_msg_close(&msg))
        {
            fprintf_light_red(stderr, "Failure closing zmq "
                                      "message.\n");
            return -1;
        }
    }

    if (old_inode->i_mtime != new_inode->i_mtime)
    {
        fprintf_yellow(stdout, "inode mtime modified.\n");
    
        val.type = BSON_STRING;
        val.size = strlen("inode.i_mtime");
        val.key = "type";
        val.data = "inode.i_mtime";

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "old";
        old = old_inode->i_mtime;
        val.data = &(old);

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "new";
        new = new_inode->i_mtime;
        val.data = &(new);

        bson_serialize(bson, &val);
        bson_finalize(bson);
        
        channel_name = construct_channel_name(vmname, path);
        buf = malloc(bson->size + strlen(channel_name) + 1);
        memcpy((uint8_t*) mempcpy(buf, channel_name, strlen(channel_name) + 1),
                                bson->buffer, bson->size);

        if (zmq_msg_init_data(&msg, buf, bson->size +
                              strlen(channel_name) + 1, 
                              qemu_free, 0))
        {
            fprintf_light_red(stderr, "Failure initializing "
                                      "zmq message data.\n");
            return -1;
        }

        free(channel_name);
        bson_reset(bson);

        if (zmq_send(pub_socket, &msg, 0))
        {
            fprintf_light_red(stderr, "Failure sending zmq "
                                      "message.\n");
            zmq_send_print_error(errno);
            return -1;
        }

        if (zmq_msg_close(&msg))
        {
            fprintf_light_red(stderr, "Failure closing zmq "
                                      "message.\n");
            return -1;
        }
    }
        
    if (old_inode->i_dtime != new_inode->i_dtime)
    {
        fprintf_yellow(stdout, "inode dtime modified.\n");

        val.type = BSON_STRING;
        val.size = strlen("inode.i_dtime");
        val.key = "type";
        val.data = "inode.i_dtime";

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "old";
        old = old_inode->i_dtime;
        val.data = &(old);

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "new";
        new = new_inode->i_dtime;
        val.data = &(new);

        bson_serialize(bson, &val);
        bson_finalize(bson);
        
        channel_name = construct_channel_name(vmname, path);
        buf = malloc(bson->size + strlen(channel_name) + 1);
        memcpy((uint8_t*) mempcpy(buf, channel_name, strlen(channel_name) + 1),
                                bson->buffer, bson->size);

        if (zmq_msg_init_data(&msg, buf, bson->size +
                              strlen(channel_name) + 1, 
                              qemu_free, 0))
        {
            fprintf_light_red(stderr, "Failure initializing "
                                      "zmq message data.\n");
            return -1;
        }

        free(channel_name);
        bson_reset(bson);

        if (zmq_send(pub_socket, &msg, 0))
        {
            fprintf_light_red(stderr, "Failure sending zmq "
                                      "message.\n");
            zmq_send_print_error(errno);
            return -1;
        }

        if (zmq_msg_close(&msg))
        {
            fprintf_light_red(stderr, "Failure closing zmq "
                                      "message.\n");
            return -1;
        }
    }

    if (old_inode->i_gid != new_inode->i_gid)
    {
        fprintf_yellow(stdout, "inode group modified.\n");

        val.type = BSON_STRING;
        val.size = strlen("inode.i_gid");
        val.key = "type";
        val.data = "inode.i_gid";

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "old";
        old = old_inode->i_gid;
        val.data = &(old);

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "new";
        new = new_inode->i_gid;
        val.data = &(new);

        bson_serialize(bson, &val);
        bson_finalize(bson);
        
        channel_name = construct_channel_name(vmname, path);
        buf = malloc(bson->size + strlen(channel_name) + 1);
        memcpy((uint8_t*) mempcpy(buf, channel_name, strlen(channel_name) + 1),
                                bson->buffer, bson->size);

        if (zmq_msg_init_data(&msg, buf, bson->size +
                              strlen(channel_name) + 1, 
                              qemu_free, 0))
        {
            fprintf_light_red(stderr, "Failure initializing "
                                      "zmq message data.\n");
            return -1;
        }

        free(channel_name);
        bson_reset(bson);

        if (zmq_send(pub_socket, &msg, 0))
        {
            fprintf_light_red(stderr, "Failure sending zmq "
                                      "message.\n");
            zmq_send_print_error(errno);
            return -1;
        }

        if (zmq_msg_close(&msg))
        {
            fprintf_light_red(stderr, "Failure closing zmq "
                                      "message.\n");
            return -1;
        }
    }

    if (old_inode->i_links_count != new_inode->i_links_count)
    {
        fprintf_yellow(stdout, "inode links count modified.\n");

        val.type = BSON_STRING;
        val.size = strlen("inode.i_links_count");
        val.key = "type";
        val.data = "inode.i_links_count";

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "old";
        old = old_inode->i_links_count;
        val.data = &(old);

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "new";
        new = new_inode->i_links_count;
        val.data = &(new);

        bson_serialize(bson, &val);
        bson_finalize(bson);
        
        channel_name = construct_channel_name(vmname, path);
        buf = malloc(bson->size + strlen(channel_name) + 1);
        memcpy((uint8_t*) mempcpy(buf, channel_name, strlen(channel_name) + 1),
                                bson->buffer, bson->size);

        if (zmq_msg_init_data(&msg, buf, bson->size +
                              strlen(channel_name) + 1, 
                              qemu_free, 0))
        {
            fprintf_light_red(stderr, "Failure initializing "
                                      "zmq message data.\n");
            return -1;
        }

        free(channel_name);
        bson_reset(bson);

        if (zmq_send(pub_socket, &msg, 0))
        {
            fprintf_light_red(stderr, "Failure sending zmq "
                                      "message.\n");
            zmq_send_print_error(errno);
            return -1;
        }

        if (zmq_msg_close(&msg))
        {
            fprintf_light_red(stderr, "Failure closing zmq "
                                      "message.\n");
            return -1;
        }
    }

    if (old_inode->i_blocks != new_inode->i_blocks)
    {
        fprintf_light_yellow(stdout, "inode block count modified.\n");
    
        val.type = BSON_STRING;
        val.size = strlen("inode.i_blocks");
        val.key = "type";
        val.data = "inode.i_blocks";

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "old";
        old = old_inode->i_blocks;
        val.data = &(old);

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "new";
        new = new_inode->i_blocks;
        val.data = &(new);

        bson_serialize(bson, &val);
        bson_finalize(bson);
        
        channel_name = construct_channel_name(vmname, path);
        buf = malloc(bson->size + strlen(channel_name) + 1);
        memcpy((uint8_t*) mempcpy(buf, channel_name, strlen(channel_name) + 1),
                                bson->buffer, bson->size);

        if (zmq_msg_init_data(&msg, buf, bson->size +
                              strlen(channel_name) + 1, 
                              qemu_free, 0))
        {
            fprintf_light_red(stderr, "Failure initializing "
                                      "zmq message data.\n");
            return -1;
        }

        free(channel_name);
        bson_reset(bson);

        if (zmq_send(pub_socket, &msg, 0))
        {
            fprintf_light_red(stderr, "Failure sending zmq "
                                      "message.\n");
            zmq_send_print_error(errno);
            return -1;
        }

        if (zmq_msg_close(&msg))
        {
            fprintf_light_red(stderr, "Failure closing zmq "
                                      "message.\n");
            return -1;
        }
    }

    if (old_inode->i_flags != new_inode->i_flags)
    {
        fprintf_yellow(stdout, "inode flags modified.\n");

        val.type = BSON_STRING;
        val.size = strlen("inode.i_flags");
        val.key = "type";
        val.data = "inode.i_flags";

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "old";
        old = old_inode->i_flags;
        val.data = &(old);

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "new";
        new = new_inode->i_flags;
        val.data = &(new);

        bson_serialize(bson, &val);
        bson_finalize(bson);
        
        channel_name = construct_channel_name(vmname, path);
        buf = malloc(bson->size + strlen(channel_name) + 1);
        memcpy((uint8_t*) mempcpy(buf, channel_name, strlen(channel_name) + 1),
                                bson->buffer, bson->size);

        if (zmq_msg_init_data(&msg, buf, bson->size +
                              strlen(channel_name) + 1, 
                              qemu_free, 0))
        {
            fprintf_light_red(stderr, "Failure initializing "
                                      "zmq message data.\n");
            return -1;
        }

        free(channel_name);
        bson_reset(bson);

        if (zmq_send(pub_socket, &msg, 0))
        {
            fprintf_light_red(stderr, "Failure sending zmq "
                                      "message.\n");
            zmq_send_print_error(errno);
            return -1;
        }

        if (zmq_msg_close(&msg))
        {
            fprintf_light_red(stderr, "Failure closing zmq "
                                      "message.\n");
            return -1;
        }
    }

    if (old_inode->i_osd1 != new_inode->i_osd1)
    {
        fprintf_yellow(stdout, "inode osd1 modified.\n");
    
        val.type = BSON_STRING;
        val.size = strlen("inode.i_osd1");
        val.key = "type";
        val.data = "inode.i_osd1";

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "old";
        old = old_inode->i_osd1;
        val.data = &(old);

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "new";
        new = new_inode->i_osd1;
        val.data = &(new);

        bson_serialize(bson, &val);
        bson_finalize(bson);
        
        channel_name = construct_channel_name(vmname, path);
        buf = malloc(bson->size + strlen(channel_name) + 1);
        memcpy((uint8_t*) mempcpy(buf, channel_name, strlen(channel_name) + 1),
                                bson->buffer, bson->size);

        if (zmq_msg_init_data(&msg, buf, bson->size +
                              strlen(channel_name) + 1, 
                              qemu_free, 0))
        {
            fprintf_light_red(stderr, "Failure initializing "
                                      "zmq message data.\n");
            return -1;
        }

        free(channel_name);
        bson_reset(bson);

        if (zmq_send(pub_socket, &msg, 0))
        {
            fprintf_light_red(stderr, "Failure sending zmq "
                                      "message.\n");
            zmq_send_print_error(errno);
            return -1;
        }

        if (zmq_msg_close(&msg))
        {
            fprintf_light_red(stderr, "Failure closing zmq "
                                      "message.\n");
            return -1;
        }
    }

    /* loop 15 */
    for (i = 0; i < 15; i++)
    {
        if (old_inode->i_block[i] == 0 && new_inode->i_block[i] != 0)
            fprintf_light_yellow(stdout, "inode block position %d [%"PRIu32"] added.\n", i, new_inode->i_block[i]);
        else if (old_inode->i_block[i] != 0 && new_inode->i_block[i] == 0)
            fprintf_light_yellow(stdout, "inode block position %d [%"PRIu32"->%"PRIu32"] removed.\n", i, old_inode->i_block[i], new_inode->i_block[i]);
        else if (old_inode->i_block[i] != new_inode->i_block[i])
            fprintf_light_yellow(stdout, "inode block position %d [%"PRIu32"->%"PRIu32"] overwritten.\n", i, old_inode->i_block[i], new_inode->i_block[i]);


        if (old_inode->i_block[i] != new_inode->i_block[i])
        {
            val.type = BSON_STRING;
            val.size = strlen("inode.i_block");
            val.key = "type";
            val.data = "inode.i_block";

            bson_serialize(bson, &val);

            val.type = BSON_INT64;
            val.key = "index";
            val.data = &(i);

            bson_serialize(bson, &val);

            val.type = BSON_INT64;
            val.key = "old";
            old = old_inode->i_block[i];
            val.data = &(old);

            bson_serialize(bson, &val);

            val.type = BSON_INT64;
            val.key = "new";
            new = new_inode->i_block[i];
            val.data = &(new);

            bson_serialize(bson, &val);
            bson_finalize(bson);
            
            channel_name = construct_channel_name(vmname, path);
            buf = malloc(bson->size + strlen(channel_name) + 1);
            memcpy((uint8_t*) mempcpy(buf, channel_name, strlen(channel_name) + 1),
                                    bson->buffer, bson->size);

            if (zmq_msg_init_data(&msg, buf, bson->size +
                                  strlen(channel_name) + 1, 
                                  qemu_free, 0))
            {
                fprintf_light_red(stderr, "Failure initializing "
                                          "zmq message data.\n");
                return -1;
            }

            free(channel_name);
            bson_reset(bson);

            if (zmq_send(pub_socket, &msg, 0))
            {
                fprintf_light_red(stderr, "Failure sending zmq "
                                          "message.\n");
                zmq_send_print_error(errno);
                return -1;
            }

            if (zmq_msg_close(&msg))
            {
                fprintf_light_red(stderr, "Failure closing zmq "
                                          "message.\n");
                return -1;
            }
        }
    }

    if (old_inode->i_generation != new_inode->i_generation)
    {
        fprintf_yellow(stdout, "inode generation modified.\n");

        val.type = BSON_STRING;
        val.size = strlen("inode.i_generation");
        val.key = "type";
        val.data = "inode.i_generation";

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "old";
        old = old_inode->i_generation;
        val.data = &(old);

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "new";
        new = new_inode->i_generation;
        val.data = &(new);

        bson_serialize(bson, &val);
        bson_finalize(bson);
        
        channel_name = construct_channel_name(vmname, path);
        buf = malloc(bson->size + strlen(channel_name) + 1);
        memcpy((uint8_t*) mempcpy(buf, channel_name, strlen(channel_name) + 1),
                                bson->buffer, bson->size);

        if (zmq_msg_init_data(&msg, buf, bson->size +
                              strlen(channel_name) + 1, 
                              qemu_free, 0))
        {
            fprintf_light_red(stderr, "Failure initializing "
                                      "zmq message data.\n");
            return -1;
        }

        free(channel_name);
        bson_reset(bson);

        if (zmq_send(pub_socket, &msg, 0))
        {
            fprintf_light_red(stderr, "Failure sending zmq "
                                      "message.\n");
            zmq_send_print_error(errno);
            return -1;
        }

        if (zmq_msg_close(&msg))
        {
            fprintf_light_red(stderr, "Failure closing zmq "
                                      "message.\n");
            return -1;
        }
    }

    if (old_inode->i_file_acl != new_inode->i_file_acl)
    {
        fprintf_yellow(stdout, "inode file_acl modified.\n");

        val.type = BSON_STRING;
        val.size = strlen("inode.i_file_acl");
        val.key = "type";
        val.data = "inode.i_file_acl";

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "old";
        old = old_inode->i_file_acl;
        val.data = &(old);

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "new";
        new = new_inode->i_file_acl;
        val.data = &(new);

        bson_serialize(bson, &val);
        bson_finalize(bson);
        
        channel_name = construct_channel_name(vmname, path);
        buf = malloc(bson->size + strlen(channel_name) + 1);
        memcpy((uint8_t*) mempcpy(buf, channel_name, strlen(channel_name) + 1),
                                bson->buffer, bson->size);

        if (zmq_msg_init_data(&msg, buf, bson->size +
                              strlen(channel_name) + 1, 
                              qemu_free, 0))
        {
            fprintf_light_red(stderr, "Failure initializing "
                                      "zmq message data.\n");
            return -1;
        }

        free(channel_name);
        bson_reset(bson);

        if (zmq_send(pub_socket, &msg, 0))
        {
            fprintf_light_red(stderr, "Failure sending zmq "
                                      "message.\n");
            zmq_send_print_error(errno);
            return -1;
        }

        if (zmq_msg_close(&msg))
        {
            fprintf_light_red(stderr, "Failure closing zmq "
                                      "message.\n");
            return -1;
        }
    }

    if (old_inode->i_dir_acl != new_inode->i_dir_acl)
    {
        fprintf_yellow(stdout, "inode dir_acl modified.\n");

        val.type = BSON_STRING;
        val.size = strlen("inode.i_dir_acl");
        val.key = "type";
        val.data = "inode.i_dir_acl";

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "old";
        old = old_inode->i_dir_acl;
        val.data = &(old);

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "new";
        new = new_inode->i_dir_acl;
        val.data = &(new);

        bson_serialize(bson, &val);
        bson_finalize(bson);
        
        channel_name = construct_channel_name(vmname, path);
        buf = malloc(bson->size + strlen(channel_name) + 1);
        memcpy((uint8_t*) mempcpy(buf, channel_name, strlen(channel_name) + 1),
                                bson->buffer, bson->size);

        if (zmq_msg_init_data(&msg, buf, bson->size +
                              strlen(channel_name) + 1, 
                              qemu_free, 0))
        {
            fprintf_light_red(stderr, "Failure initializing "
                                      "zmq message data.\n");
            return -1;
        }

        free(channel_name);
        bson_reset(bson);

        if (zmq_send(pub_socket, &msg, 0))
        {
            fprintf_light_red(stderr, "Failure sending zmq "
                                      "message.\n");
            zmq_send_print_error(errno);
            return -1;
        }

        if (zmq_msg_close(&msg))
        {
            fprintf_light_red(stderr, "Failure closing zmq "
                                      "message.\n");
            return -1;
        }
    }

    if (old_inode->i_faddr != new_inode->i_faddr)
    {
        fprintf_yellow(stdout, "inode faddr modified.\n");

        val.type = BSON_STRING;
        val.size = strlen("inode.i_faddr");
        val.key = "type";
        val.data = "inode.i_faddr";

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "old";
        old = old_inode->i_faddr;
        val.data = &(old);

        bson_serialize(bson, &val);

        val.type = BSON_INT64;
        val.key = "new";
        new = new_inode->i_faddr;
        val.data = &(new);

        bson_serialize(bson, &val);
        bson_finalize(bson);
        
        channel_name = construct_channel_name(vmname, path);
        buf = malloc(bson->size + strlen(channel_name) + 1);
        memcpy((uint8_t*) mempcpy(buf, channel_name, strlen(channel_name) + 1),
                                bson->buffer, bson->size);

        if (zmq_msg_init_data(&msg, buf, bson->size +
                              strlen(channel_name) + 1, 
                              qemu_free, 0))
        {
            fprintf_light_red(stderr, "Failure initializing "
                                      "zmq message data.\n");
            return -1;
        }

        free(channel_name);
        bson_reset(bson);

        if (zmq_send(pub_socket, &msg, 0))
        {
            fprintf_light_red(stderr, "Failure sending zmq "
                                      "message.\n");
            zmq_send_print_error(errno);
            return -1;
        }

        if (zmq_msg_close(&msg))
        {
            fprintf_light_red(stderr, "Failure closing zmq "
                                      "message.\n");
            return -1;
        }
    }

    for (i = 0; i < 12; i++)
    {
        if (old_inode->i_osd2[i] != new_inode->i_osd2[i])
        {
            fprintf_yellow(stdout, "inode osd2 byte %d modified.\n", i);

            val.type = BSON_STRING;
            val.size = strlen("inode.i_osd2");
            val.key = "type";
            val.data = "inode.i_osd2";

            bson_serialize(bson, &val);

            val.type = BSON_INT64;
            val.key = "index";
            val.data = &(i);

            bson_serialize(bson, &val);

            val.type = BSON_INT64;
            val.key = "old";
            old = old_inode->i_osd2[i];
            val.data = &(old);

            bson_serialize(bson, &val);

            val.type = BSON_INT64;
            val.key = "new";
            new = new_inode->i_osd2[i];
            val.data = &(new);

            bson_serialize(bson, &val);
            bson_finalize(bson);
            
            channel_name = construct_channel_name(vmname, path);
            buf = malloc(bson->size + strlen(channel_name) + 1);
            memcpy((uint8_t*) mempcpy(buf, channel_name, strlen(channel_name) + 1),
                                    bson->buffer, bson->size);

            if (zmq_msg_init_data(&msg, buf, bson->size +
                                  strlen(channel_name) + 1, 
                                  qemu_free, 0))
            {
                fprintf_light_red(stderr, "Failure initializing "
                                          "zmq message data.\n");
                return -1;
            }

            free(channel_name);
            bson_reset(bson);

            if (zmq_send(pub_socket, &msg, 0))
            {
                fprintf_light_red(stderr, "Failure sending zmq "
                                          "message.\n");
                zmq_send_print_error(errno);
                return -1;
            }

            if (zmq_msg_close(&msg))
            {
                fprintf_light_red(stderr, "Failure closing zmq "
                                          "message.\n");
                return -1;
            }
        }
    }
    
    *old_inode = *new_inode;

    bson_cleanup(bson);

    return EXIT_SUCCESS;
}

int qemu_deep_inspect(struct qemu_bdrv_write* write, struct mbr* mbr,
                      void* pub_socket, char* vmname)
{
    uint64_t i, j, start = 0, end = 0;
    uint64_t inode_offset;
    uint8_t* buf;
    char* channel_name;
    struct partition* partition;
    struct ext2_fs* fs;
    struct ext2_file* file;
    zmq_msg_t msg;
    struct ext2_inode new_inode;
    struct bson_info* bson;
    struct bson_kv val;

    for (i = 0; i < linkedlist_size(mbr->pt); i++)
    {
        partition = linkedlist_get(mbr->pt, i);
        
        if (write->header.sector_num <= partition->final_sector_lba &&
            write->header.sector_num >= partition->first_sector_lba)
        {
            fs = &(partition->fs);
            for (j = 0; j < linkedlist_size(fs->ext2_files); j++)
            {
                file = linkedlist_get(fs->ext2_files, j);

                if (file->inode_sector >= write->header.sector_num &&
                    file->inode_sector <= write->header.sector_num +
                                          write->header.nb_sectors - 1)
                {
                    fprintf_light_red(stdout, "Write to sector %"PRIu64
                                              " containing inode for file "
                                              "%s\n", file->inode_sector,
                                              file->path);

                    inode_offset = (file->inode_sector -
                                    write->header.sector_num) * 512;
                    inode_offset += file->inode_offset;

                    new_inode = *((struct ext2_inode*)
                                  &(write->data[inode_offset]));

                    /* compare inode, emit diff */
                    ext2_compare_inodes(&(file->inode), &new_inode, pub_socket,
                                        vmname, file->path);
                }

                if (bst_find(file->sectors, write->header.sector_num))
                {
                    fprintf_light_red(stdout, "Write to sector %"PRId64
                                              " modifying %s\n",
                                              write->header.sector_num,
                                              file->path);
                    if (file->is_dir)
                        fprintf_light_green(stdout, "Directory modification."
                                                    "\n");

                    if (!file->is_dir)
                    {
                        bson = bson_init();

                        val.type = BSON_STRING;
                        val.size = strlen(FILE_DATA_WRITE); 
                        val.key = "type";
                        val.data = FILE_DATA_WRITE;

                        bson_serialize(bson, &val);

                        val.type = BSON_INT64;
                        val.key = "start_byte";
                        val.data = &start;

                        bson_serialize(bson, &val);

                        val.type = BSON_INT64;
                        val.key = "end_byte";
                        val.data = &end;

                        bson_serialize(bson, &val);

                        val.type = BSON_BINARY;
                        val.subtype = BSON_BINARY_GENERIC;
                        val.key = "data";
                        val.data = write->data;
                        val.size = write->header.nb_sectors * SECTOR_SIZE;

                        bson_serialize(bson, &val);
                        bson_finalize(bson);

                        channel_name = construct_channel_name(vmname,
                                                              file->path);

                        buf = malloc(bson->size + strlen(channel_name) + 1);
                        memcpy((uint8_t*) mempcpy(buf, channel_name,
                                                  strlen(channel_name) + 1),
                                bson->buffer, bson->size);

                        if (zmq_msg_init_data(&msg, buf, bson->size +
                                                         strlen(channel_name) + 1, 
                                              qemu_free, 0))
                        {
                            fprintf_light_red(stderr, "Failure initializing "
                                                      "zmq message data.\n");
                            return -1;
                        }

                        free(channel_name);
                        bson_cleanup(bson);

                        if (zmq_send(pub_socket, &msg, 0))
                        {
                            fprintf_light_red(stderr, "Failure sending zmq "
                                                      "message.\n");
                            zmq_send_print_error(errno);
                            return -1;
                        }

                        if (zmq_msg_close(&msg))
                        {
                            fprintf_light_red(stderr, "Failure closing zmq "
                                                      "message.\n");
                            return -1;
                        }
                    }
                }
            }

            return SECTOR_EXT2_PARTITION;
        }
    }

   return 0;
}

int qemu_infer_sector_type(struct qemu_bdrv_write* write, struct mbr* mbr)
{
    uint64_t i, j;
    struct partition* partition;
    struct ext2_fs* fs;
    struct ext2_bgd* bgd;

    uint64_t block_size, blocks_per_block_group, sectors_per_block_group,
             start_sector, bgd_start, bgd_end;

    if (write->header.sector_num == mbr->sector)
        return SECTOR_MBR;

    for (i = 0; i < linkedlist_size(mbr->pt); i++)
    {
        partition = linkedlist_get(mbr->pt, i);
        
        if (write->header.sector_num <= partition->final_sector_lba &&
            write->header.sector_num >= partition->first_sector_lba)
        {
            if (write->header.sector_num == partition->first_sector_lba + 2)
                return SECTOR_EXT2_SUPERBLOCK;

            fs = &(partition->fs);
            block_size = ext2_block_size(fs->superblock);
            blocks_per_block_group = fs->superblock.s_blocks_per_group;
            sectors_per_block_group = blocks_per_block_group *
                                      (block_size / SECTOR_SIZE);
            start_sector = fs->superblock.s_first_data_block *
                           (block_size / SECTOR_SIZE) + partition->first_sector_lba;

            for (j = 0; j < linkedlist_size(fs->ext2_bgds); j++)
            {
                bgd_start = start_sector + sectors_per_block_group * j;
                bgd_end = bgd_start + sectors_per_block_group - 1;
                bgd = linkedlist_get(fs->ext2_bgds, j);
                if (write->header.sector_num == bgd->sector)
                    return SECTOR_EXT2_BLOCK_GROUP_DESCRIPTOR;

                if (write->header.sector_num <= bgd->block_bitmap_sector_end &&
                    write->header.sector_num >= bgd->block_bitmap_sector_start)
                    return SECTOR_EXT2_BLOCK_GROUP_BLOCKMAP;

                if (write->header.sector_num <= bgd->inode_bitmap_sector_end &&
                    write->header.sector_num >= bgd->inode_bitmap_sector_start)
                    return SECTOR_EXT2_BLOCK_GROUP_INODEMAP;

                if (write->header.sector_num <= bgd->inode_table_sector_end &&
                    write->header.sector_num >= bgd->inode_table_sector_start)
                    return SECTOR_EXT2_INODE;

                if (write->header.sector_num <= bgd_end &&
                    write->header.sector_num >= bgd_start)
                    return SECTOR_EXT2_DATA;

            }

            return SECTOR_EXT2_PARTITION;
        }
    }

   return SECTOR_UNKNOWN;
}

int qemu_print_write(struct qemu_bdrv_write* write)
{
    fprintf_light_blue(stdout, "brdv_write event\n");
    fprintf_yellow(stdout, "\tsector_num: %0."PRId64"\n",
                           write->header.sector_num);
    fprintf_yellow(stdout, "\tnb_sectors: %d\n",
                           write->header.nb_sectors);
    fprintf_yellow(stdout, "\tdata buffer pointer (malloc()'d): %p\n",
                           write->data);
    return 0;
}

void print_ext2_file(struct ext2_file* file)
{
    fprintf_light_cyan(stdout, "-- ext2 File --\n");
    fprintf_yellow(stdout, "file->inode_sector == %"PRIu64"\n", file->inode_sector);
    fprintf_yellow(stdout, "file->inode_offset == %"PRIu64"\n", file->inode_offset);
    fprintf_light_yellow(stdout, "file->path == %s\n", file->path);
    fprintf_yellow(stdout, "file->is_dir == %s\n", file->is_dir ? "true" : "false");
    fprintf_yellow(stdout, "file->inode == %p\n", &(file->inode));
    if (file->sectors)
        bst_print_tree(file->sectors, 0);
    else
        fprintf_light_blue(stdout, "No sectors -- empty file\n");
}

void print_ext2_bgd(struct ext2_bgd* bgd)
{
    fprintf_light_cyan(stdout, "-- ext2 BGD --\n");
    fprintf_yellow(stdout, "bgd->bgd == %p\n", &(bgd->bgd));
    fprintf_yellow(stdout, "bgd->sector == %"PRIu64"\n", bgd->sector);
    fprintf_yellow(stdout, "bgd->block_bitmap_sector_start == %"PRIu64"\n", bgd->block_bitmap_sector_start);
    fprintf_yellow(stdout, "bgd->block_bitmap_sector_end == %"PRIu64"\n", bgd->block_bitmap_sector_end);
    fprintf_yellow(stdout, "bgd->inode_bitmap_sector_start == %"PRIu64"\n", bgd->inode_bitmap_sector_start);
    fprintf_yellow(stdout, "bgd->inode_bitmap_sector_end == %"PRIu64"\n", bgd->inode_bitmap_sector_end);
    fprintf_yellow(stdout, "bgd->inode_table_sector_start == %"PRIu64"\n", bgd->inode_table_sector_start);
    fprintf_yellow(stdout, "bgd->inode_table_sector_end == %"PRIu64"\n", bgd->inode_table_sector_end);
}

void print_ext2_fs(struct ext2_fs* fs)
{
    struct ext2_bgd* bgd;
    struct ext2_file* file;
    uint64_t i;

    fprintf_light_cyan(stdout, "-- ext2 FS --\n");
    fprintf_yellow(stdout, "fs->fs_type %"PRIu64"\n", fs->fs_type);
    fprintf_yellow(stdout, "fs->mount_point %s\n", fs->mount_point);
    fprintf_yellow(stdout, "fs->num_block_groups %"PRIu64"\n", fs->num_block_groups);
    fprintf_yellow(stdout, "fs->num_files %"PRIu64"\n", fs->num_files);
    
    for (i = 0; i < linkedlist_size(fs->ext2_bgds); i++)
    {
        bgd = linkedlist_get(fs->ext2_bgds, i);
        print_ext2_bgd(bgd);
    }

    for (i = 0; i < linkedlist_size(fs->ext2_files); i++)
    {
        file = linkedlist_get(fs->ext2_files, i);
        print_ext2_file(file);
    }
}

void print_partition(struct linkedlist* pt)
{
    struct partition* pte;
    uint64_t i;

    for (i = 0; i < linkedlist_size(pt); i++)
    {
        pte = linkedlist_get(pt, i);
        fprintf_light_cyan(stdout, "-- Partition --\n");
        fprintf_yellow(stdout, "pte->pte_num == %"PRIu64"\n", pte->pte_num);
        fprintf_yellow(stdout, "pte->partition_type == %"PRIu64"\n", pte->partition_type);
        fprintf_yellow(stdout, "pte->first_sector_lba == %"PRIu64"\n", pte->first_sector_lba);
        fprintf_yellow(stdout, "pte->final_sector_lba == %"PRIu64"\n", pte->final_sector_lba);
        fprintf_yellow(stdout, "pte->sector == %"PRIu64"\n", pte->sector);
        fprintf_yellow(stdout, "pte->fs == %p\n", &(pte->fs));
        print_ext2_fs(&(pte->fs));
    }
}

void print_mbr(struct mbr* mbr)
{
    fprintf_light_cyan(stdout, "-- MBR --\n");
    fprintf_yellow(stdout, "mbr->gpt == %d\n", mbr->gpt);
    fprintf_yellow(stdout, "mbr->sector == %"PRIu64"\n", mbr->sector);
    fprintf_yellow(stdout, "mbr->active_partitions == %"PRIu64"\n", mbr->active_partitions);
    fprintf_yellow(stdout, "mbr->pt == %p\n", mbr->pt);
    print_partition(mbr->pt);
}


int __deserialize_mbr(FILE* index, struct bson_info* bson, struct mbr* mbr)
{
    struct bson_kv value1, value2;

    if (bson_readf(bson, index) != 1)
        return EXIT_FAILURE;

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "gpt") != 0)
        return EXIT_FAILURE;

    mbr->gpt = *((bool*) value1.data);

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "sector") != 0)
        return EXIT_FAILURE;

    mbr->sector = *((uint32_t*) value1.data);

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "active_partitions") != 0)
        return EXIT_FAILURE;

    mbr->active_partitions = *((uint32_t*) value1.data);
    mbr->pt = linkedlist_init();
    return EXIT_SUCCESS;
}

int __deserialize_partition(FILE* index, struct bson_info* bson,
                            struct mbr* mbr)
{
    struct partition pt;
    struct bson_kv value1, value2;

    if (bson_readf(bson, index) != 1)
        return EXIT_FAILURE;

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "pte_num") != 0)
        return EXIT_FAILURE;

    pt.pte_num = *((uint32_t*) value1.data);

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "partition_type") != 0)
        return EXIT_FAILURE;

    pt.partition_type = *((uint32_t*) value1.data);    
    
    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "first_sector_lba") != 0)
        return EXIT_FAILURE;

    pt.first_sector_lba = *((uint32_t*) value1.data);    

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "final_sector_lba") != 0)
        return EXIT_FAILURE;

    pt.final_sector_lba = *((uint32_t*) value1.data);    
    
    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "sector") != 0)
        return EXIT_FAILURE;

    pt.sector = *((uint32_t*) value1.data);

    linkedlist_append(mbr->pt, &pt, sizeof(pt));

    return EXIT_SUCCESS;
}

int __deserialize_ext2_fs(FILE* index, struct bson_info* bson,
                          struct partition* pt)
{
    struct ext2_fs fs;
    struct bson_kv value1, value2;

    fs.ext2_bgds = linkedlist_init();
    fs.ext2_files = linkedlist_init();

    if (bson_readf(bson, index) != 1)
        return EXIT_FAILURE;

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "fs_type") != 0)
        return EXIT_FAILURE;

    fs.fs_type = *((uint32_t*) value1.data);

    if (fs.fs_type != 0)
        return EXIT_FAILURE;

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "mount_point") != 0)
        return EXIT_FAILURE;

    fs.mount_point = clone_cstring((char*) value1.data);

    if (fs.mount_point == NULL)
        return EXIT_FAILURE;

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "num_block_groups") != 0)
        return EXIT_FAILURE;

    fs.num_block_groups = *((uint32_t*) value1.data);

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "num_files") != 0)
        return EXIT_FAILURE;

    fs.num_files = *((uint32_t*) value1.data);

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "superblock") != 0)
        return EXIT_FAILURE;

    fs.superblock = *((struct ext2_superblock*) value1.data);

    pt->fs = fs;

    return EXIT_SUCCESS;
}

int __deserialize_ext2_bgd(FILE* index, struct bson_info* bson,
                           struct ext2_fs* fs)
{
    struct ext2_bgd bgd;
    struct bson_kv value1, value2;

    if (bson_readf(bson, index) != 1)
        return EXIT_FAILURE;

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "bgd") != 0)
        return EXIT_FAILURE;

    bgd.bgd = *((struct ext2_block_group_descriptor*) value1.data);

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "sector") != 0)
        return EXIT_FAILURE;

    bgd.sector = *((uint32_t*) value1.data);

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "block_bitmap_sector_start") != 0)
        return EXIT_FAILURE;

    bgd.block_bitmap_sector_start = *((uint32_t*) value1.data);

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "block_bitmap_sector_end") != 0)
        return EXIT_FAILURE;

    bgd.block_bitmap_sector_end = *((uint32_t*) value1.data);

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "inode_bitmap_sector_start") != 0)
        return EXIT_FAILURE;

    bgd.inode_bitmap_sector_start = *((uint32_t*) value1.data);

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "inode_bitmap_sector_end") != 0)
        return EXIT_FAILURE;

    bgd.inode_bitmap_sector_end = *((uint32_t*) value1.data);

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "inode_table_sector_start") != 0)
        return EXIT_FAILURE;

    bgd.inode_table_sector_start = *((uint32_t*) value1.data);

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "inode_table_sector_end") != 0)
        return EXIT_FAILURE;

    bgd.inode_table_sector_end = *((uint32_t*) value1.data);

    linkedlist_append(fs->ext2_bgds, &bgd, sizeof(bgd));

    return EXIT_SUCCESS;
}

int __deserialize_ext2_file(FILE* index, struct bson_info* bson,
                            struct ext2_fs* fs)
{
    struct ext2_file file;
    struct bson_kv value1, value2;

    if (bson_readf(bson, index) != 1)
        return EXIT_FAILURE;

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "inode_sector") != 0)
        return EXIT_FAILURE;

    file.inode_sector = *((uint64_t*) value1.data);

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "inode_offset") != 0)
        return EXIT_FAILURE;

    file.inode_offset = *((uint64_t*) value1.data);

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "path") != 0)
        return EXIT_FAILURE;

    file.path = clone_cstring((char*) value1.data);

    if (file.path == NULL)
        return EXIT_FAILURE;

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "is_dir") != 0)
        return EXIT_FAILURE;

    file.is_dir = *((bool*) value1.data);

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "inode") != 0)
        return EXIT_FAILURE;

    file.inode = *((struct ext2_inode*) value1.data);

    if (bson_deserialize(bson, &value1, &value2) != 1)
        return EXIT_FAILURE;
    
    if (strcmp(value1.key, "sectors") != 0)
        return EXIT_FAILURE;

    bson = bson_init();
    bson->buffer = malloc(value2.size);

    if (bson->buffer == NULL)
        return EXIT_FAILURE;
    
    memcpy(bson->buffer, value2.data, value2.size);
    bson_make_readable(bson);
    file.sectors = NULL;

    /* at least 1 sector */
    if (bson_deserialize(bson, &value1, &value2) == 1)
    { 
        file.sectors = bst_init(*((uint32_t*)value1.data), (void*)1);
        if (file.sectors == NULL)
            return EXIT_FAILURE;
    }

    while (bson_deserialize(bson, &value1, &value2) == 1)
    {
        bst_insert(file.sectors, *((uint32_t*)value1.data), (void*) 1);
    }

    linkedlist_append(fs->ext2_files, &file, sizeof(file));
    bson_cleanup(bson);

    return EXIT_SUCCESS;
}

int qemu_load_index(FILE* index, struct mbr* mbr)
{
    uint64_t i, j;
    struct bson_info* bson;
    struct partition* pt;

    bson = bson_init();

    /* mbr */
    if (__deserialize_mbr(index, bson, mbr))
    {
        fprintf_light_red(stderr, "Error loading MBR document.\n");
        return EXIT_FAILURE;
    }

    /* partition entries */
    for (i = 0; i < mbr->active_partitions; i++)
    {
        if (__deserialize_partition(index, bson, mbr))
        {
            fprintf_light_red(stderr, "Error loading partition document.\n");
            return EXIT_FAILURE;
        }

        pt = (struct partition*) linkedlist_tail(mbr->pt);

        if (__deserialize_ext2_fs(index, bson, pt))
        {
            fprintf_light_red(stderr, "Error loading ext2_fs document.\n");
            return EXIT_FAILURE;
        }

        for (j = 0; j < pt->fs.num_block_groups; j++)
        {
            if (__deserialize_ext2_bgd(index, bson, &(pt->fs)))
            {
                fprintf_light_red(stderr, "Error loading ext2_bgd document."
                                          "\n");
                return EXIT_FAILURE;
            }
        }        

        for (j = 0; j < pt->fs.num_files; j++)
        {
            if (__deserialize_ext2_file(index, bson, &(pt->fs)))
            {
                fprintf_light_red(stderr, "Error loading ext2_file document."
                                          "\n");
                return EXIT_FAILURE;
            }
        }
    } 

    //print_mbr(mbr);
    bson_cleanup(bson);

    return EXIT_SUCCESS;
}