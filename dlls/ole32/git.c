/*
 * Implementation of the StdGlobalInterfaceTable object
 *
 * The GlobalInterfaceTable (GIT) object is used to marshal interfaces between
 * threading apartments (contexts). When you want to pass an interface but not
 * as a parameter, it wouldn't get marshalled automatically, so you can use this
 * object to insert the interface into a table, and you get back a cookie.
 * Then when it's retrieved, it'll be unmarshalled into the right apartment.
 *
 * Copyright 2003 Mike Hearn <mike@theoretic.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "windef.h"
#include "objbase.h"
#include "ole2.h"
#include "winbase.h"
#include "winerror.h"
#include "winternl.h"

#include "compobj_private.h" 

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ole);

/****************************************************************************
 * StdGlobalInterfaceTable definition
 *
 * This class implements IGlobalInterfaceTable and is a process-wide singleton
 * used for marshalling interfaces between threading apartments using cookies.
 */

/* Each entry in the linked list of GIT entries */
typedef struct StdGITEntry
{
  DWORD cookie;
  IID iid;      /* IID of the interface */
  IStream* stream; /* Holds the marshalled interface */

  struct StdGITEntry* next;
  struct StdGITEntry* prev;  
} StdGITEntry;

/* Class data */
typedef struct StdGlobalInterfaceTableImpl
{
  ICOM_VFIELD(IGlobalInterfaceTable);

  ULONG ref;
  struct StdGITEntry* firstEntry;
  struct StdGITEntry* lastEntry;
  ULONG nextCookie;
  
} StdGlobalInterfaceTableImpl;


/* IUnknown */
static HRESULT WINAPI StdGlobalInterfaceTable_QueryInterface(IGlobalInterfaceTable* iface, REFIID riid, void** ppvObject);
static ULONG   WINAPI StdGlobalInterfaceTable_AddRef(IGlobalInterfaceTable* iface);
static ULONG   WINAPI StdGlobalInterfaceTable_Release(IGlobalInterfaceTable* iface);
/* IGlobalInterfaceTable */
static HRESULT WINAPI StdGlobalInterfaceTable_RegisterInterfaceInGlobal(IGlobalInterfaceTable* iface, IUnknown* pUnk, REFIID riid, DWORD* pdwCookie);
static HRESULT WINAPI StdGlobalInterfaceTable_RevokeInterfaceFromGlobal(IGlobalInterfaceTable* iface, DWORD dwCookie);
static HRESULT WINAPI StdGlobalInterfaceTable_GetInterfaceFromGlobal(IGlobalInterfaceTable* iface, DWORD dwCookie, REFIID riid, void **ppv);

/* Virtual function table */
static ICOM_VTABLE(IGlobalInterfaceTable) StdGlobalInterfaceTableImpl_Vtbl =
{
  ICOM_MSVTABLE_COMPAT_DummyRTTIVALUE
  StdGlobalInterfaceTable_QueryInterface,
  StdGlobalInterfaceTable_AddRef,
  StdGlobalInterfaceTable_Release,
  StdGlobalInterfaceTable_RegisterInterfaceInGlobal,
  StdGlobalInterfaceTable_RevokeInterfaceFromGlobal,
  StdGlobalInterfaceTable_GetInterfaceFromGlobal
};

/***
 * Let's go! Here is the constructor and destructor for the class.
 *
 */

/** This function constructs the GIT. It should only be called once **/
void* StdGlobalInterfaceTable_Construct() {
  StdGlobalInterfaceTableImpl* newGIT;

  newGIT = HeapAlloc(GetProcessHeap(), 0, sizeof(StdGlobalInterfaceTableImpl));
  if (newGIT == 0) return newGIT;

  ICOM_VTBL(newGIT) = &StdGlobalInterfaceTableImpl_Vtbl;
  
  newGIT->ref = 0;      /* Initialise the reference count */
  newGIT->firstEntry = NULL; /* we start with an empty table   */
  newGIT->lastEntry  = NULL;
  newGIT->nextCookie = 0xf100; /* that's where windows starts, so that's where we start */
  TRACE("Created the GIT at %p\n", newGIT);

  return (void*)newGIT;
}

/** This destroys it again. It should revoke all the held interfaces first **/
void StdGlobalInterfaceTable_Destroy(void* self) {
  TRACE("(%p)\n", self);
  FIXME("Revoke held interfaces here\n");
  
  HeapFree(GetProcessHeap(), 0, self);
}

/***
 * A helper function to traverse the list and find the entry that matches the cookie.
 * Returns NULL if not found
 */
StdGITEntry* StdGlobalInterfaceTable_FindEntry(IGlobalInterfaceTable* iface, DWORD cookie) {
  StdGlobalInterfaceTableImpl* const self = (StdGlobalInterfaceTableImpl*) iface;
  StdGITEntry* e;

  TRACE("iface=%p, cookie=0x%x\n", iface, (UINT)cookie);
  
  e = self->firstEntry;
  while (e != NULL) {
    if (e->cookie == cookie) return e;
    e = e->next;
  }
  return NULL;
}

/***
 * Here's the boring boilerplate stuff for IUnknown
 */

HRESULT WINAPI StdGlobalInterfaceTable_QueryInterface(IGlobalInterfaceTable* iface, REFIID riid, void** ppvObject) {
  StdGlobalInterfaceTableImpl* const self = (StdGlobalInterfaceTableImpl*) iface;

  /* Make sure silly coders can't crash us */
  if (ppvObject == 0) return E_INVALIDARG;

  *ppvObject = 0; /* assume we don't have the interface */

  /* Do we implement that interface? */
  if (IsEqualIID(&IID_IUnknown, riid)) {
    *ppvObject = (IGlobalInterfaceTable*) self;
  } else if (IsEqualIID(&IID_IGlobalInterfaceTable, riid)) {
    *ppvObject = (IGlobalInterfaceTable*) self;
  } else return E_NOINTERFACE;

  /* Now inc the refcount */
  /* we don't use refcounts for now: StdGlobalInterfaceTable_AddRef(iface); */
  return S_OK;
}

ULONG WINAPI StdGlobalInterfaceTable_AddRef(IGlobalInterfaceTable* iface) {
  StdGlobalInterfaceTableImpl* const self = (StdGlobalInterfaceTableImpl*) iface;

  self->ref++;
  return self->ref;
}

ULONG WINAPI StdGlobalInterfaceTable_Release(IGlobalInterfaceTable* iface) {
  StdGlobalInterfaceTableImpl* const self = (StdGlobalInterfaceTableImpl*) iface;

  self->ref--;
  if (self->ref == 0) {
    /* Hey ho, it's time to go, so long again 'till next weeks show! */
    StdGlobalInterfaceTable_Destroy(self);
    return 0;
  }

  return self->ref;
}

/***
 * Now implement the actual IGlobalInterfaceTable interface
 */

HRESULT WINAPI StdGlobalInterfaceTable_RegisterInterfaceInGlobal(IGlobalInterfaceTable* iface, IUnknown* pUnk, REFIID riid, DWORD* pdwCookie) {
  StdGlobalInterfaceTableImpl* const self = (StdGlobalInterfaceTableImpl*) iface;
  IStream* stream = NULL;
  HRESULT hres;
  StdGITEntry* entry;

  TRACE("iface=%p, pUnk=%p, riid=%s, pdwCookie=%p\n", iface, pUnk, debugstr_guid(riid), pdwCookie);

  if (pUnk == NULL) return E_INVALIDARG;
  
  /* marshal the interface */
  hres = CoMarshalInterThreadInterfaceInStream(riid, pUnk, &stream);
  if (hres) return hres;
  entry = HeapAlloc(GetProcessHeap(), 0, sizeof(StdGITEntry));
  if (entry == NULL) return E_OUTOFMEMORY;
  
  entry->iid = *riid;
  entry->stream = stream;
  entry->cookie = self->nextCookie;
  self->nextCookie++; /* inc the cookie count */

  /* insert the new entry at the end of the list */
  entry->next = NULL;
  entry->prev = self->lastEntry;
  if (entry->prev) entry->prev->next = entry;
  else self->firstEntry = entry;
  self->lastEntry = entry;

  /* and return the cookie */
  *pdwCookie = entry->cookie;
  return S_OK;
}

HRESULT WINAPI StdGlobalInterfaceTable_RevokeInterfaceFromGlobal(IGlobalInterfaceTable* iface, DWORD dwCookie) {
  StdGlobalInterfaceTableImpl* const self = (StdGlobalInterfaceTableImpl*) iface;
  StdGITEntry* entry;

  TRACE("iface=%p, dwCookie=0x%x\n", iface, (UINT)dwCookie);
  
  entry = StdGlobalInterfaceTable_FindEntry(iface, dwCookie);
  if (entry == NULL) {
    TRACE("Entry not found\n");
    return E_INVALIDARG; /* not found */
  }

  /* chop entry out of the list, and free the memory */
  if (entry->prev) entry->prev->next = entry->next;
  else self->firstEntry = entry->next;
  if (entry->next) entry->next->prev = entry->prev;
  else self->lastEntry = entry->prev;
  HeapFree(GetProcessHeap(), 0, entry);
  return S_OK;
}

HRESULT WINAPI StdGlobalInterfaceTable_GetInterfaceFromGlobal(IGlobalInterfaceTable* iface, DWORD dwCookie, REFIID riid, void **ppv) {
  StdGITEntry* entry;
  HRESULT hres;
  
  entry = StdGlobalInterfaceTable_FindEntry(iface, dwCookie);
  if (entry == NULL) return E_INVALIDARG;
  if (!IsEqualIID(&entry->iid, &riid)) return E_INVALIDARG;

  /* unmarshal the interface */
  hres = CoGetInterfaceAndReleaseStream(entry->stream, riid, *ppv);
  if (hres) return hres;
  
  return S_OK;
}
