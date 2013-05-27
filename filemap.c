/*
 *  Copyright (C) 2013 Masatoshi Teruya
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */
/*
 *  filemap.c
 *  Created by Masatoshi Teruya on 13/05/27.
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <lauxlib.h>
#include <lualib.h>

#define FILEMAP_LUA     "filemap"
#define PATH_TMPL       "/tmp/filemap-XXXXXX"
#define EMMAP           ((void*)-1)
#define ELSEEK          ((off_t)-1)

typedef struct {
    int fd;
    size_t csize;
    size_t rsize;
} filemap_t;

#define pusherror(L,msg)  (lua_settop(L,0),lua_pushnil(L),lua_pushstring(L,msg))

static int init_lua( lua_State *L )
{
    char tmpl[] = PATH_TMPL;
    filemap_t *fmap = lua_newuserdata( L, sizeof( filemap_t ) );
    
    if( !fmap ){
        return luaL_error( L, "failed to init(): %s", lua_tostring( L, -1 ) );
    }
    luaL_getmetatable( L, FILEMAP_LUA );
    lua_setmetatable( L, -2 );
    
    fmap->csize = 0;
    fmap->rsize = 0;
    // create descriptor
    if( ( fmap->fd = mkstemp( tmpl ) ) == -1 ){
        pusherror( L, strerror( errno ) );
        return 2;
    }
    else if( fcntl( fmap->fd, F_SETFD, FD_CLOEXEC ) == -1 ){
        close( fmap->fd );
        pusherror( L, strerror( errno ) );
        return 2;
    }
    
    return 1;
}

static filemap_t *fmap_getself( lua_State *L )
{
    int argc = lua_gettop( L );
    filemap_t *fmap = NULL;
    void *udata = NULL;
    
    if( argc && ( udata = lua_touserdata( L, 1 ) ) )
    {
        // validate metatable
        if( lua_getmetatable( L, 1 ) )
        {
            lua_pushliteral( L, FILEMAP_LUA );
            lua_rawget( L, LUA_REGISTRYINDEX );
            if( lua_rawequal( L, -1, -2 ) ){
                fmap = (filemap_t*)udata;
            }
        }
    }
    
    lua_settop( L, argc );
    return fmap;
}

static int fd_lua( lua_State *L )
{
    filemap_t *fmap = fmap_getself( L );
     
    if( !fmap ){
        pusherror( L, "argument#1 must be instance of " FILEMAP_LUA );
    }
    else {
        lua_pushinteger( L, fmap->fd );
        lua_pushnil( L );
    }
    
    return 2;
}

static int fmap_append( filemap_t *fmap, int fd, size_t size, size_t *head )
{
    int rc = -1;
    size_t rsize = fmap->csize + fmap->rsize + size;
    
    // resizing
    if( ftruncate( fmap->fd, rsize ) != -1 )
    {
        char *min = NULL;
        
        // rewind
        lseek( fd, 0, SEEK_SET );
        min = (void*)mmap( NULL, size, PROT_READ, MAP_SHARED, fd, 0 );
        if( min != EMMAP )
        {
            char *mout = (void*)mmap( NULL, rsize, PROT_WRITE, MAP_SHARED, fmap->fd, 0 );
            
            if( mout != EMMAP )
            {
                size_t i = 0;
                
                *head = fmap->csize + fmap->rsize;
                // copy
                for( i = 0; i < size; i++ ){
                    mout[*head+i] = min[i];
                }
                msync( mout, rsize, MS_SYNC );
                munmap( mout, rsize );
                fmap->rsize += size;
                rc = 0;
            }
            munmap( min, size );
        }
    }
    
    return rc;
}

static int add_lua( lua_State *L )
{
    filemap_t *fmap = fmap_getself( L );
    char *errmsg = NULL;
     
    if( !fmap ){
        errmsg = "argument#1 must be instance of " FILEMAP_LUA;
    }
    else if( lua_type( L, 2 ) != LUA_TSTRING ){
        errmsg = "argument#2 must be filepath";
    }
    else
    {
        int fd = 0;
        size_t size = 0;
        size_t head = 0;
        size_t len = 0;
        const char *path = lua_tolstring( L, 2, &len );
        
        if( !len ){
            errmsg = "argument#2 invalid path lentgth";
        }
        // append file data
        else if( ( fd = open( path, O_RDONLY ) ) == -1 || 
            ( size = lseek( fd, 0, SEEK_END ) ) == ELSEEK ||
            fmap_append( fmap, fd, size, &head ) == -1 ){
            errmsg = strerror( errno );
        }
        else {
            lua_newtable( L );
            lua_pushstring( L, "head" );
            lua_pushinteger( L, head );
            lua_rawset( L, -3 );
            lua_pushstring( L, "size" );
            lua_pushinteger( L, size );
            lua_rawset( L, -3 );
            lua_pushnil( L );
        }
        
        // cleanup
        if( fd ){
            close( fd );
        }
    }
    
    if( errmsg ){
        pusherror( L, errmsg );
    }
    
    return 2;
}

static int rewind_lua( lua_State *L )
{
    filemap_t *fmap = lua_touserdata( L, 1 );
    
    // rewind
    lseek( fmap->fd, 0, SEEK_SET );
    
    return 0;
}

static int tostring_lua( lua_State *L )
{
    filemap_t *fmap = lua_touserdata( L, 1 );
    
    lua_pushfstring( L, FILEMAP_LUA ": %p", fmap );
    
    return 1;
}

static int gc_lua( lua_State *L )
{
    filemap_t *fmap = lua_touserdata( L, 1 );
    
    if( fmap->fd ){
        close( fmap->fd );
    }
    
    return 0;
}

LUALIB_API int luaopen_filemap( lua_State *L )
{
    struct luaL_Reg metaReg[] = {
        { "__gc", gc_lua },
        { "__tostring", tostring_lua },
        { NULL, NULL }
    };
    struct luaL_Reg methodReg[] = {
        { "fd", fd_lua },
        { "add", add_lua },
        { "rewind", rewind_lua },
        { NULL, NULL }
    };
    int top = lua_gettop( L );
    int i = 0;
    
    // metatable for userdata
    luaL_newmetatable( L, FILEMAP_LUA );
    // add metamethods
    for( i = 0; metaReg[i].name; i++ ){ 
        lua_pushstring( L, metaReg[i].name );
        lua_pushcfunction( L, metaReg[i].func );
        lua_rawset( L, -3 );
    }
    // add method
    lua_pushstring( L, "__index" );
    lua_newtable( L );
    for( i = 0; methodReg[i].name; i++ ){ 
        lua_pushstring( L, methodReg[i].name );
        lua_pushcfunction( L, methodReg[i].func );
        lua_rawset( L, -3 );
    }
    lua_rawset( L, -3 );
    lua_settop( L, top );
    
    // exports
    lua_newtable( L );
    lua_pushstring( L, "init" );
    lua_pushcfunction( L, init_lua );
    lua_rawset( L, -3 );
    
    return 1;
}

