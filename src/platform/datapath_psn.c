/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    QUIC datapath Abstraction Layer.

Environment:

    Linux

--*/

#include "platform_internal.h"
#include <inttypes.h>
#ifdef QUIC_CLOG
#include "datapath_psn.c.clog.h"
#endif

//
// Not yet available in the SDK. When available this code can be removed.
//
#if 1
#define UDP_SEND_MSG_SIZE           2
#define UDP_RECV_MAX_COALESCED_SIZE 3
#define UDP_COALESCED_INFO          3
#endif

CXPLAT_STATIC_ASSERT(
    sizeof(QUIC_BUFFER) == sizeof(WSABUF),
    "WSABUF is assumed to be interchangeable for QUIC_BUFFER");
CXPLAT_STATIC_ASSERT(
    FIELD_OFFSET(QUIC_BUFFER, Length) == FIELD_OFFSET(WSABUF, len),
    "WSABUF is assumed to be interchangeable for QUIC_BUFFER");
CXPLAT_STATIC_ASSERT(
    FIELD_OFFSET(QUIC_BUFFER, Buffer) == FIELD_OFFSET(WSABUF, buf),
    "WSABUF is assumed to be interchangeable for QUIC_BUFFER");

//
// The maximum single buffer size for sending coalesced payloads.
//
#define CXPLAT_LARGE_SEND_BUFFER_SIZE         0xFFFF

//
// If we have UDP segmentation support, use a single batch. Without UDP
// segmentation, increase batch size to gain back some performance
//
#define CXPLAT_MAX_BATCH_SEND 1

//
// The maximum UDP receive coalescing payload.
//
#define MAX_URO_PAYLOAD_LENGTH              (UINT16_MAX - CXPLAT_UDP_HEADER_SIZE)

//
// A receive block to receive a UDP packet over the sockets.
//
typedef struct CXPLAT_DATAPATH_RECV_BLOCK {
    //
    // The pool owning this recv block.
    //
    CXPLAT_POOL* OwningPool;

    //
    // The recv buffer used by MsQuic.
    //
    CXPLAT_RECV_DATA RecvPacket;

    //
    // Represents the network route.
    //
    CXPLAT_ROUTE Route;

    //
    // Buffer that actually stores the UDP payload.
    //
    uint8_t Buffer[MAX_UDP_PAYLOAD_LENGTH];

    //
    // This follows the recv block.
    //
    // CXPLAT_RECV_PACKET RecvContext;

} CXPLAT_DATAPATH_RECV_BLOCK;

//
// Send context.
//

typedef struct CXPLAT_SEND_DATA {
    //
    // Indicates if the send should be bound to a local address.
    //
    BOOLEAN Bind;

    //
    // The local address to bind to.
    //
    QUIC_ADDR LocalAddress;

    //
    // The remote address to send to.
    //
    QUIC_ADDR RemoteAddress;

    //
    // Linkage to pending send list.
    //
    CXPLAT_LIST_ENTRY PendingSendLinkage;

    //
    // The type of ECN markings needed for send.
    //
    CXPLAT_ECN_TYPE ECN;

    //
    // The proc context owning this send context.
    //
    struct CXPLAT_DATAPATH_PROC_CONTEXT *Owner;

    //
    // The send segmentation size; zero if segmentation is not performed.
    //
    uint16_t SegmentSize;

    //
    // The total buffer size for Buffers.
    //
    uint32_t TotalSize;

    //
    // BufferCount - The buffer count in use.
    //
    // CurrentIndex - The current index of the Buffers to be sent.
    //
    // Buffers - Send buffers.
    //
    // Iovs - IO vectors used for doing sends on the socket.
    //
    // TODO: Better way to reconcile layout difference
    // between QUIC_BUFFER and struct iovec?
    //
    size_t BufferCount;
    QUIC_BUFFER Buffers[CXPLAT_MAX_BATCH_SEND];

    //
    // The QUIC_BUFFER returned to the client for segmented sends.
    //
    QUIC_BUFFER ClientBuffer;

} CXPLAT_SEND_DATA;

typedef struct CXPLAT_RECV_MSG_CONTROL_BUFFER {
    char Data[CMSG_SPACE(sizeof(struct in6_pktinfo)) +
              CMSG_SPACE(sizeof(struct in_pktinfo)) +
              2 * CMSG_SPACE(sizeof(int))];
} CXPLAT_RECV_MSG_CONTROL_BUFFER;

typedef struct CXPLAT_DATAPATH_PROC_CONTEXT CXPLAT_DATAPATH_PROC_CONTEXT;

//
// Socket context.
//
typedef struct CXPLAT_SOCKET_CONTEXT {

    //
    // The datapath binding this socket context belongs to.
    //
    CXPLAT_SOCKET* Binding;

    //
    // The datapath proc context this socket belongs to.
    //
    CXPLAT_DATAPATH_PROC_CONTEXT* ProcContext;

    //
    // The socket FD used by this socket context.
    //
    SOCKET SocketFd;

    //
    // Used to register different event FD with PSN.
    //
#define QUIC_SOCK_EVENT_CLEANUP 0
#define QUIC_SOCK_EVENT_SOCKET  1
    uint8_t EventContexts[2];

    //
    // The I/O vector for receive datagrams.
    //
    struct iovec RecvIov[CXPLAT_MAX_BATCH_RECEIVE];

    //
    // The control buffer used in RecvMsgHdr.
    //
    CXPLAT_RECV_MSG_CONTROL_BUFFER RecvMsgControl[CXPLAT_MAX_BATCH_RECEIVE];

    //
    // The buffer used to receive msg headers on socket.
    //
    struct mmsghdr RecvMsgHdr[CXPLAT_MAX_BATCH_RECEIVE];

    //
    // The receive block currently being used for receives on this socket.
    //
    CXPLAT_DATAPATH_RECV_BLOCK* CurrentRecvBlocks[CXPLAT_MAX_BATCH_RECEIVE];

    //
    // The head of list containg all pending sends on this socket.
    //
    CXPLAT_LIST_ENTRY PendingSendDataHead;

    //
    // Lock around the PendingSendData list.
    //
    CXPLAT_LOCK PendingSendDataLock;

} CXPLAT_SOCKET_CONTEXT;

//
// Datapath binding.
//
typedef struct CXPLAT_SOCKET {

    //
    // Synchronization mechanism for cleanup.
    // Make sure events are in front for cache alignment.
    //
    CXPLAT_RUNDOWN_REF Rundown;

    //
    // A pointer to datapath object.
    //
    CXPLAT_DATAPATH* Datapath;

    //
    // The client context for this binding.
    //
    void *ClientContext;

    //
    // The local address for the binding.
    //
    QUIC_ADDR LocalAddress;

    //
    //  The remote address for the binding.
    //
    QUIC_ADDR RemoteAddress;

    //
    // Indicates the binding connected to a remote IP address.
    //
    BOOLEAN Connected : 1;

    //
    // Indicates the binding is shut down.
    //
    BOOLEAN Shutdown : 1;

    //
    // Flag indicates the socket has a default remote destination.
    //
    BOOLEAN HasFixedRemoteAddress : 1;

    //
    // Flag indicates the binding is being used for PCP.
    //
    BOOLEAN PcpBinding : 1;

    //
    // The MTU for this binding.
    //
    uint16_t Mtu;

    //
    // Set of socket contexts one per proc.
    //
    CXPLAT_SOCKET_CONTEXT SocketContexts[];

} CXPLAT_SOCKET;

//
// A per processor datapath context.
//
typedef struct CXPLAT_DATAPATH_PROC_CONTEXT {

    //
    // A pointer to the datapath.
    //
    CXPLAT_DATAPATH* Datapath;

    //
    // The IOCP for this proc context.
    //
    HANDLE CompletionPort;

    //
    // The index of the context in the datapath's array.
    //
    uint32_t Index;

    //
    // Thread ID of the worker thread that drives execution.
    //
    CXPLAT_THREAD_ID ThreadId;

    //
    // Completion event to indicate the worker has cleaned up.
    //
    CXPLAT_EVENT CompletionEvent;

    //
    // Pool of receive packet contexts and buffers to be shared by all sockets
    // on this core.
    //
    CXPLAT_POOL RecvBlockPool;

    //
    // Pool of send buffers to be shared by all sockets on this core.
    //
    CXPLAT_POOL SendBufferPool;

    //
    // Pool of large segmented send buffers to be shared by all sockets on this
    // core.
    //
    CXPLAT_POOL LargeSendBufferPool;

    //
    // Pool of send data contexts to be shared by all sockets on this core.
    //
    CXPLAT_POOL SendDataPool;

} CXPLAT_DATAPATH_PROC_CONTEXT;

//
// Represents a datapath object.
//

typedef struct CXPLAT_DATAPATH {

    //
    // A reference rundown on the datapath binding.
    // Make sure events are in front for cache alignment.
    //
    CXPLAT_RUNDOWN_REF BindingsRundown;

    //
    // Set of supported features.
    //
    uint32_t Features;

    //
    // If datapath is shutting down.
    //
    BOOLEAN volatile Shutdown;

    //
    // The max send batch size.
    // TODO: See how send batching can be enabled.
    //
    uint8_t MaxSendBatchSize;

    //
    // UDP handlers.
    //
    CXPLAT_UDP_DATAPATH_CALLBACKS UdpHandlers;

    //
    // The length of recv context used by MsQuic.
    //
    size_t ClientRecvContextLength;

    //
    // The proc count to create per proc datapath state.
    //
    uint32_t ProcCount;

    //
    // Function pointer to WSASendMsg.
    //
    LPFN_WSASENDMSG WSASendMsg;

    //
    // Function pointer to WSARecvMsg.
    //
    LPFN_WSARECVMSG WSARecvMsg;

    //
    // The per proc datapath contexts.
    //
    CXPLAT_DATAPATH_PROC_CONTEXT ProcContexts[];

} CXPLAT_DATAPATH;

QUIC_STATUS
CxPlatSocketSendInternal(
    _In_ CXPLAT_SOCKET* Socket,
    _In_ const QUIC_ADDR* LocalAddress,
    _In_ const QUIC_ADDR* RemoteAddress,
    _In_ CXPLAT_SEND_DATA* SendData,
    _In_ BOOLEAN IsPendedSend
    );

QUIC_STATUS
CxPlatDataPathQuerySockoptSupport(
    _Inout_ CXPLAT_DATAPATH* Datapath
    )
{
    int Result;
    int OptionLength;
    DWORD BytesReturned;
    GUID WSASendMsgGuid = WSAID_WSASENDMSG;
    GUID WSARecvMsgGuid = WSAID_WSARECVMSG;
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;

    SOCKET UdpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (UdpSocket == INVALID_SOCKET) {
        int WsaError = WSAGetLastError();
        QuicTraceLogWarning(
            DatapathOpenUdpSocketFailed,
            "[data] UDP send segmentation helper socket failed to open, 0x%x",
            WsaError);
        goto Error;
    }

    Result =
        WSAIoctl(
            UdpSocket,
            SIO_GET_EXTENSION_FUNCTION_POINTER,
            &WSASendMsgGuid,
            sizeof(WSASendMsgGuid),
            &Datapath->WSASendMsg,
            sizeof(Datapath->WSASendMsg),
            &BytesReturned,
            NULL,
            NULL);
    if (Result != NO_ERROR) {
        int WsaError = WSAGetLastError();
        QuicTraceEvent(
            LibraryErrorStatus,
            "[ lib] ERROR, %u, %s.",
            WsaError,
            "SIO_GET_EXTENSION_FUNCTION_POINTER (WSASendMsg)");
        Status = HRESULT_FROM_WIN32(WsaError);
        goto Error;
    }

    Result =
        WSAIoctl(
            UdpSocket,
            SIO_GET_EXTENSION_FUNCTION_POINTER,
            &WSARecvMsgGuid,
            sizeof(WSARecvMsgGuid),
            &Datapath->WSARecvMsg,
            sizeof(Datapath->WSARecvMsg),
            &BytesReturned,
            NULL,
            NULL);
    if (Result != NO_ERROR) {
        int WsaError = WSAGetLastError();
        QuicTraceEvent(
            LibraryErrorStatus,
            "[ lib] ERROR, %u, %s.",
            WsaError,
            "SIO_GET_EXTENSION_FUNCTION_POINTER (WSARecvMsg)");
        Status = HRESULT_FROM_WIN32(WsaError);
        goto Error;
    }

#ifdef UDP_SEND_MSG_SIZE
{
    DWORD SegmentSize;
    OptionLength = sizeof(SegmentSize);
    Result =
        getsockopt(
            UdpSocket,
            IPPROTO_UDP,
            UDP_SEND_MSG_SIZE,
            (char*)&SegmentSize,
            &OptionLength);
    if (Result != NO_ERROR) {
        int WsaError = WSAGetLastError();
        QuicTraceLogWarning(
            DatapathQueryUdpSendMsgFailed,
            "[data] Query for UDP_SEND_MSG_SIZE failed, 0x%x",
            WsaError);
    } else {
        Datapath->Features |= CXPLAT_DATAPATH_FEATURE_SEND_SEGMENTATION;
    }
}
#endif

#ifdef UDP_RECV_MAX_COALESCED_SIZE
{
    DWORD UroMaxCoalescedMsgSize = TRUE;
    OptionLength = sizeof(UroMaxCoalescedMsgSize);
    Result =
        getsockopt(
            UdpSocket,
            IPPROTO_UDP,
            UDP_RECV_MAX_COALESCED_SIZE,
            (char*)&UroMaxCoalescedMsgSize,
            &OptionLength);
    if (Result != NO_ERROR) {
        int WsaError = WSAGetLastError();
        QuicTraceLogWarning(
            DatapathQueryRecvMaxCoalescedSizeFailed,
            "[data] Query for UDP_RECV_MAX_COALESCED_SIZE failed, 0x%x",
            WsaError);
    } else {
        Datapath->Features |= CXPLAT_DATAPATH_FEATURE_RECV_COALESCING;
    }
}
#endif

Error:

    if (UdpSocket != INVALID_SOCKET) {
        closesocket(UdpSocket);
    }

    return Status;
}

void
CxPlatProcessorContextUninitialize(
    _In_ CXPLAT_DATAPATH_PROC_CONTEXT* ProcContext
    )
{
    PostQueuedCompletionStatus(ProcContext->CompletionPort, 0, (ULONG_PTR)NULL, NULL);
    CxPlatEventWaitForever(ProcContext->CompletionEvent);
    CxPlatEventUninitialize(ProcContext->CompletionEvent);

    CloseHandle(ProcContext->CompletionPort);

    CxPlatPoolUninitialize(&ProcContext->RecvBlockPool);
    CxPlatPoolUninitialize(&ProcContext->LargeSendBufferPool);
    CxPlatPoolUninitialize(&ProcContext->SendBufferPool);
    CxPlatPoolUninitialize(&ProcContext->SendDataPool);
}

QUIC_STATUS
CxPlatProcessorContextInitialize(
    _In_ CXPLAT_DATAPATH* Datapath,
    _In_ uint32_t Index,
    _Out_ CXPLAT_DATAPATH_PROC_CONTEXT* ProcContext
    )
{
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    HANDLE CompletionPort = INVALID_HANDLE_VALUE;
    int Ret = 0;
    uint32_t RecvPacketLength = 0;

    CXPLAT_DBG_ASSERT(Datapath != NULL);

    RecvPacketLength =
        sizeof(CXPLAT_DATAPATH_RECV_BLOCK) + Datapath->ClientRecvContextLength;

    ProcContext->Index = Index;
    CxPlatPoolInitialize(
        TRUE,
        RecvPacketLength,
        QUIC_POOL_DATA,
        &ProcContext->RecvBlockPool);
    CxPlatPoolInitialize(
        TRUE,
        MAX_UDP_PAYLOAD_LENGTH,
        QUIC_POOL_DATA,
        &ProcContext->SendBufferPool);
    CxPlatPoolInitialize(
        TRUE,
        CXPLAT_LARGE_SEND_BUFFER_SIZE,
        QUIC_POOL_DATA,
        &ProcContext->LargeSendBufferPool);
    CxPlatPoolInitialize(
        TRUE,
        sizeof(CXPLAT_SEND_DATA),
        QUIC_POOL_PLATFORM_SENDCTX,
        &ProcContext->SendDataPool);

    CompletionPort =
        CreateIoCompletionPort(
            INVALID_HANDLE_VALUE,
            NULL,
            0,
            1);

    if (CompletionPort == INVALID_HANDLE_VALUE) {
        Status = HRESULT_FROM_WIN32(GetLastError());
        QuicTraceEvent(
            LibraryErrorStatus,
            "[ lib] ERROR, %u, %s.",
            Status,
            "CreateIoCompletionPort failed");
        goto Exit;
    }

    ProcContext->Datapath = Datapath;
    ProcContext->CompletionPort = CompletionPort;
    ProcContext->ThreadId = 0;

    //
    // Starting the thread must be done after the rest of the ProcContext
    // members have been initialized. Because the thread start routine accesses
    // ProcContext members.
    //

    CxPlatEventInitialize(&ProcContext->CompletionEvent, TRUE, FALSE);
    CxPlatWorkerRegisterDataPath((uint16_t)Index, ProcContext);

Exit:

    if (QUIC_FAILED(Status)) {
        if (CompletionPort != INVALID_HANDLE_VALUE) {
            CxPlatCloseHandle(CompletionPort);
        }
        CxPlatPoolUninitialize(&ProcContext->RecvBlockPool);
        CxPlatPoolUninitialize(&ProcContext->LargeSendBufferPool);
        CxPlatPoolUninitialize(&ProcContext->SendBufferPool);
        CxPlatPoolUninitialize(&ProcContext->SendDataPool);
    }

    return Status;
}

QUIC_STATUS
CxPlatDataPathInitialize(
    _In_ uint32_t ClientRecvContextLength,
    _In_opt_ const CXPLAT_UDP_DATAPATH_CALLBACKS* UdpCallbacks,
    _In_opt_ const CXPLAT_TCP_DATAPATH_CALLBACKS* TcpCallbacks,
    _In_opt_ CXPLAT_DATAPATH_CONFIG* Config,
    _Out_ CXPLAT_DATAPATH** NewDataPath
    )
{
    UNREFERENCED_PARAMETER(TcpCallbacks);
    UNREFERENCED_PARAMETER(Config);
    if (NewDataPath == NULL) {
        return QUIC_STATUS_INVALID_PARAMETER;
    }
    if (UdpCallbacks != NULL) {
        if (UdpCallbacks->Receive == NULL || UdpCallbacks->Unreachable == NULL) {
            return QUIC_STATUS_INVALID_PARAMETER;
        }
    }

    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;

    size_t DatapathLength =
        sizeof(CXPLAT_DATAPATH) +
            CxPlatProcMaxCount() * sizeof(CXPLAT_DATAPATH_PROC_CONTEXT);

    CXPLAT_DATAPATH* Datapath = (CXPLAT_DATAPATH*)CXPLAT_ALLOC_PAGED(DatapathLength, QUIC_POOL_DATAPATH);
    if (Datapath == NULL) {
        QuicTraceEvent(
            AllocFailure,
            "Allocation of '%s' failed. (%llu bytes)",
            "CXPLAT_DATAPATH",
            DatapathLength);
        Status = QUIC_STATUS_OUT_OF_MEMORY;
        goto Exit;
    }

    CxPlatZeroMemory(Datapath, DatapathLength);
    if (UdpCallbacks) {
        Datapath->UdpHandlers = *UdpCallbacks;
    }
    Datapath->ClientRecvContextLength = ClientRecvContextLength;
    Datapath->ProcCount = CxPlatProcMaxCount();
    Datapath->MaxSendBatchSize = CXPLAT_MAX_BATCH_SEND;
    Datapath->Features = 0;
    CxPlatRundownInitialize(&Datapath->BindingsRundown);

    Status = CxPlatDataPathQuerySockoptSupport(Datapath);
    if (QUIC_FAILED(Status)) {
        goto Exit;
    }

    //
    // Initialize the per processor contexts.
    //
    for (uint32_t i = 0; i < Datapath->ProcCount; i++) {
        Status = CxPlatProcessorContextInitialize(Datapath, i, &Datapath->ProcContexts[i]);
        if (QUIC_FAILED(Status)) {
            Datapath->Shutdown = TRUE;
            for (uint32_t j = 0; j < i; j++) {
                CxPlatProcessorContextUninitialize(&Datapath->ProcContexts[j]);
            }
            goto Exit;
        }
    }

    *NewDataPath = Datapath;
    Datapath = NULL;

Exit:

    if (Datapath != NULL) {
        CxPlatRundownUninitialize(&Datapath->BindingsRundown);
        CXPLAT_FREE(Datapath, QUIC_POOL_DATAPATH);
    }

    return Status;
}

void
CxPlatDataPathUninitialize(
    _In_ CXPLAT_DATAPATH* Datapath
    )
{
    if (Datapath == NULL) {
        return;
    }

    CxPlatRundownReleaseAndWait(&Datapath->BindingsRundown);

    Datapath->Shutdown = TRUE;
    for (uint32_t i = 0; i < Datapath->ProcCount; i++) {
        CxPlatProcessorContextUninitialize(&Datapath->ProcContexts[i]);
    }

    CxPlatRundownUninitialize(&Datapath->BindingsRundown);
    CXPLAT_FREE(Datapath, QUIC_POOL_DATAPATH);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
uint32_t
CxPlatDataPathGetSupportedFeatures(
    _In_ CXPLAT_DATAPATH* Datapath
    )
{
    return Datapath->Features;
}

BOOLEAN
CxPlatDataPathIsPaddingPreferred(
    _In_ CXPLAT_DATAPATH* Datapath
    )
{
    return !!(Datapath->Features & CXPLAT_DATAPATH_FEATURE_SEND_SEGMENTATION);
}

CXPLAT_DATAPATH_RECV_BLOCK*
CxPlatDataPathAllocRecvBlock(
    _In_ CXPLAT_DATAPATH_PROC_CONTEXT* DatapathProc
    )
{
    CXPLAT_DATAPATH_RECV_BLOCK* RecvBlock =
        CxPlatPoolAlloc(&DatapathProc->RecvBlockPool);
    if (RecvBlock == NULL) {
        QuicTraceEvent(
            AllocFailure,
            "Allocation of '%s' failed. (%llu bytes)",
            "CXPLAT_DATAPATH_RECV_BLOCK",
            0);
    } else {
        CxPlatZeroMemory(RecvBlock, sizeof(*RecvBlock));
        RecvBlock->OwningPool = &DatapathProc->RecvBlockPool;
        RecvBlock->RecvPacket.Buffer = RecvBlock->Buffer;
        RecvBlock->RecvPacket.Allocated = TRUE;
    }
    return RecvBlock;
}

void
CxPlatDataPathPopulateTargetAddress(
    _In_ QUIC_ADDRESS_FAMILY Family,
    _In_ ADDRINFO* AddrInfo,
    _Out_ QUIC_ADDR* Address
    )
{
    struct sockaddr_in6* SockAddrIn6 = NULL;
    struct sockaddr_in* SockAddrIn = NULL;

    CxPlatZeroMemory(Address, sizeof(QUIC_ADDR));

    if (AddrInfo->ai_addr->sa_family == AF_INET6) {
        CXPLAT_DBG_ASSERT(sizeof(struct sockaddr_in6) == AddrInfo->ai_addrlen);

        //
        // Is this a mapped ipv4 one?
        //

        SockAddrIn6 = (struct sockaddr_in6*)AddrInfo->ai_addr;

        if (Family == QUIC_ADDRESS_FAMILY_UNSPEC && IN6_IS_ADDR_V4MAPPED(&SockAddrIn6->sin6_addr)) {
            SockAddrIn = &Address->Ipv4;

            //
            // Get the ipv4 address from the mapped address.
            //

            SockAddrIn->sin_family = QUIC_ADDRESS_FAMILY_INET;
            memcpy(&SockAddrIn->sin_addr.s_addr, &SockAddrIn6->sin6_addr.s6_addr[12], 4);
            SockAddrIn->sin_port = SockAddrIn6->sin6_port;

            return;
        }
        Address->Ipv6 = *SockAddrIn6;
        Address->Ipv6.sin6_family = QUIC_ADDRESS_FAMILY_INET6;
        return;
    }

    if (AddrInfo->ai_addr->sa_family == AF_INET) {
        CXPLAT_DBG_ASSERT(sizeof(struct sockaddr_in) == AddrInfo->ai_addrlen);
        SockAddrIn = (struct sockaddr_in*)AddrInfo->ai_addr;
        Address->Ipv4 = *SockAddrIn;
        Address->Ipv4.sin_family = QUIC_ADDRESS_FAMILY_INET;
        return;
    }

    CXPLAT_FRE_ASSERT(FALSE);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
_Success_(QUIC_SUCCEEDED(return))
QUIC_STATUS
CxPlatDataPathGetLocalAddresses(
    _In_ CXPLAT_DATAPATH* Datapath,
    _Outptr_ _At_(*Addresses, __drv_allocatesMem(Mem))
        CXPLAT_ADAPTER_ADDRESS** Addresses,
    _Out_ uint32_t* AddressesCount
    )
{
    UNREFERENCED_PARAMETER(Datapath);
    *Addresses = NULL;
    *AddressesCount = 0;
    return QUIC_STATUS_NOT_SUPPORTED;
}

QUIC_STATUS
CxPlatDataPathGetGatewayAddresses(
    _In_ CXPLAT_DATAPATH* Datapath,
    _Outptr_ _At_(*GatewayAddresses, __drv_allocatesMem(Mem))
        QUIC_ADDR** GatewayAddresses,
    _Out_ uint32_t* GatewayAddressesCount
    )
{
    UNREFERENCED_PARAMETER(Datapath);
    *GatewayAddresses = NULL;
    *GatewayAddressesCount = 0;
    return QUIC_STATUS_NOT_SUPPORTED;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
CxPlatDataPathResolveAddress(
    _In_ CXPLAT_DATAPATH* Datapath,
    _In_z_ const char* HostName,
    _Inout_ QUIC_ADDR* Address
    )
{
    QUIC_STATUS Status;
    PWSTR HostNameW = NULL;
    ADDRINFOW Hints = { 0 };
    ADDRINFOW *Ai;

    Status =
        CxPlatUtf8ToWideChar(
            HostName,
            QUIC_POOL_PLATFORM_TMP_ALLOC,
            &HostNameW);
    if (QUIC_FAILED(Status)) {
        QuicTraceEvent(
            LibraryErrorStatus,
            "[ lib] ERROR, %u, %s.",
            Status,
            "Convert HostName to unicode");
        goto Exit;
    }

    //
    // Prepopulate hint with input family. It might be unspecified.
    //
    Hints.ai_family = Address->si_family;

    //
    // Try numeric name first.
    //
    Hints.ai_flags = AI_NUMERICHOST;
    if (GetAddrInfoW(HostNameW, NULL, &Hints, &Ai) == 0) {
        CxPlatDataPathPopulateTargetAddress((ADDRESS_FAMILY)Hints.ai_family, Ai, Address);
        FreeAddrInfoW(Ai);
        Status = QUIC_STATUS_SUCCESS;
        goto Exit;
    }

    //
    // Try canonical host name.
    //
    Hints.ai_flags = AI_CANONNAME;
    if (GetAddrInfoW(HostNameW, NULL, &Hints, &Ai) == 0) {
        CxPlatDataPathPopulateTargetAddress((ADDRESS_FAMILY)Hints.ai_family, Ai, Address);
        FreeAddrInfoW(Ai);
        Status = QUIC_STATUS_SUCCESS;
        goto Exit;
    }

    QuicTraceEvent(
        LibraryError,
        "[ lib] ERROR, %s.",
        "Resolving hostname to IP");
    QuicTraceLogError(
        DatapathResolveHostNameFailed,
        "[%p] Couldn't resolve hostname '%s' to an IP address",
        Datapath,
        HostName);
    Status = HRESULT_FROM_WIN32(WSAHOST_NOT_FOUND);

Exit:

    if (HostNameW != NULL) {
        CXPLAT_FREE(HostNameW, QUIC_POOL_PLATFORM_TMP_ALLOC);
    }

    return Status;
}

QUIC_STATUS
CxPlatSocketConfigureRss(
    _In_ CXPLAT_SOCKET_CONTEXT* SocketContext,
    _In_ uint32_t SocketCount
    )
{
#ifdef SO_ATTACH_REUSEPORT_CBPF
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    int Result = 0;

    struct sock_filter BpfCode[] = {
        {BPF_LD | BPF_W | BPF_ABS, 0, 0, SKF_AD_OFF | SKF_AD_CPU},
        {BPF_ALU | BPF_MOD, 0, 0, SocketCount},
        {BPF_RET | BPF_A, 0, 0, 0}
    };

    struct sock_fprog BpfConfig = {
        .len = ARRAYSIZE(BpfCode),
        .filter = BpfCode
    };

    Result =
        setsockopt(
            SocketContext->SocketFd,
            SOL_SOCKET,
            SO_ATTACH_REUSEPORT_CBPF,
            (const void*)&BpfConfig,
            sizeof(BpfConfig));
    if (Result == SOCKET_ERROR) {
        Status = errno;
        QuicTraceEvent(
            DatapathErrorStatus,
            "[data][%p] ERROR, %u, %s.",
            SocketContext->Binding,
            Status,
            "setsockopt(SO_ATTACH_REUSEPORT_CBPF) failed");
    }

    return Status;
#else
    UNREFERENCED_PARAMETER(SocketContext);
    UNREFERENCED_PARAMETER(SocketCount);
    return QUIC_STATUS_NOT_SUPPORTED;
#endif
}

//
// Socket context interface. It abstracts a (generally per-processor) UDP socket
// and the corresponding logic/functionality like send and receive processing.
//

QUIC_STATUS
CxPlatSocketContextInitialize(
    _Inout_ CXPLAT_SOCKET_CONTEXT* SocketContext,
    _In_ const QUIC_ADDR* LocalAddress,
    _In_opt_ const QUIC_ADDR* RemoteAddress,
    _In_ BOOLEAN ForceShare,
    _In_ uint16_t Index
    )
{
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    int Result = 0;
    int Option = 0;
    QUIC_ADDR MappedAddress = {0};
    socklen_t AssignedLocalAddressLength = 0;

    CXPLAT_SOCKET* Binding = SocketContext->Binding;

    for (uint32_t i = 0; i < ARRAYSIZE(SocketContext->EventContexts); ++i) {
        SocketContext->EventContexts[i] = i;
    }

    //
    // Create datagram socket.
    //
    SocketContext->SocketFd =
        socket(
            AF_INET6,
            SOCK_DGRAM,
            IPPROTO_UDP);
    if (SocketContext->SocketFd == INVALID_SOCKET) {
        int WsaError = WSAGetLastError();
        QuicTraceEvent(
            DatapathErrorStatus,
            "[data][%p] ERROR, %u, %s.",
            Binding,
            WsaError,
            "socket failed");
        Status = HRESULT_FROM_WIN32(WsaError);
        goto Exit;
    }

    //
    // Set non blocking
    //

    u_long mode = 1;
    Result =
        ioctlsocket(SocketContext->SocketFd, FIONBIO, &mode);
    if (Result == SOCKET_ERROR) {
        int WsaError = WSAGetLastError();
        QuicTraceEvent(
            DatapathErrorStatus,
            "[data][%p] ERROR, %u, %s.",
            Binding,
            WsaError,
            "ioctlsocket(FIONBIO) failed");
        Status = HRESULT_FROM_WIN32(WsaError);
        goto Exit;
    }

    //
    // Set dual (IPv4 & IPv6) socket mode.
    //
    Option = FALSE;
    Result =
        setsockopt(
            SocketContext->SocketFd,
            IPPROTO_IPV6,
            IPV6_V6ONLY,
            (const void*)&Option,
            sizeof(Option));
    if (Result == SOCKET_ERROR) {
        int WsaError = WSAGetLastError();
        QuicTraceEvent(
            DatapathErrorStatus,
            "[data][%p] ERROR, %u, %s.",
            Binding,
            WsaError,
            "setsockopt(IPV6_V6ONLY) failed");
        Status = HRESULT_FROM_WIN32(WsaError);
        goto Exit;
    }

    if (RemoteAddress == NULL) {
        uint16_t Processor = Index;
        DWORD BytesReturned = 0;
        Result =
            WSAIoctl(
                SocketContext->SocketFd,
                SIO_CPU_AFFINITY,
                &Processor,
                sizeof(Processor),
                NULL,
                0,
                &BytesReturned,
                NULL,
                NULL);
        if (Result != NO_ERROR) {
            int WsaError = WSAGetLastError();
            QuicTraceEvent(
                    DatapathErrorStatus,
                    "[data][%p] ERROR, %u, %s.",
                    Binding,
                    WsaError,
                    "SIO_CPU_AFFINITY");
            Status = HRESULT_FROM_WIN32(WsaError);
            goto Exit;
        }
    }

    //
    // Set DON'T FRAG socket option.
    //

    //
    // Windows: setsockopt IPPROTO_IP IP_DONTFRAGMENT TRUE.
    // Linux: IP_DONTFRAGMENT option is not available. IPV6_MTU_DISCOVER is the
    // apparent alternative.
    // TODO: Verify this.
    //
    Option = TRUE;
    Result =
        setsockopt(
            SocketContext->SocketFd,
            IPPROTO_IP,
            IP_DONTFRAGMENT,
            (char*)&Option,
            sizeof(Option));
    if (Result == SOCKET_ERROR) {
        int WsaError = WSAGetLastError();
        QuicTraceEvent(
            DatapathErrorStatus,
            "[data][%p] ERROR, %u, %s.",
            Binding,
            WsaError,
            "setsockopt(IP_DONTFRAGMENT) failed");
        Status = HRESULT_FROM_WIN32(WsaError);
        goto Exit;
    }

    Option = TRUE;
    Result =
        setsockopt(
            SocketContext->SocketFd,
            IPPROTO_IPV6,
            IPV6_DONTFRAG,
            (const void*)&Option,
            sizeof(Option));
    if (Result == SOCKET_ERROR) {
        int WsaError = WSAGetLastError();
        QuicTraceEvent(
            DatapathErrorStatus,
            "[data][%p] ERROR, %u, %s.",
            Binding,
            WsaError,
            "setsockopt(IPV6_DONTFRAG) failed");
        Status = HRESULT_FROM_WIN32(WsaError);
        goto Exit;
    }

    //
    // Set socket option to receive ancillary data about the incoming packets.
    //

    //
    // Windows: setsockopt IPPROTO_IPV6 IPV6_PKTINFO TRUE.
    // Android: Returns EINVAL. IPV6_PKTINFO option is not present in documentation.
    // IPV6_RECVPKTINFO seems like is the alternative.
    // TODO: Check if this works as expected?
    //
    Option = TRUE;
    Result =
        setsockopt(
            SocketContext->SocketFd,
            IPPROTO_IPV6,
            IPV6_PKTINFO,
            (const void*)&Option,
            sizeof(Option));
    if (Result == SOCKET_ERROR) {
        int WsaError = WSAGetLastError();
        QuicTraceEvent(
            DatapathErrorStatus,
            "[data][%p] ERROR, %u, %s.",
            Binding,
            WsaError,
            "setsockopt(IPV6_PKTINFO) failed");
        Status = HRESULT_FROM_WIN32(WsaError);
        goto Exit;
    }

    Option = TRUE;
    Result =
        setsockopt(
            SocketContext->SocketFd,
            IPPROTO_IP,
            IP_PKTINFO,
            (const void*)&Option,
            sizeof(Option));
    if (Result == SOCKET_ERROR) {
        int WsaError = WSAGetLastError();
        QuicTraceEvent(
            DatapathErrorStatus,
            "[data][%p] ERROR, %u, %s.",
            Binding,
            WsaError,
            "setsockopt(IP_PKTINFO) failed");
        Status = HRESULT_FROM_WIN32(WsaError);
        goto Exit;
    }

    //
    // Set socket option to receive TOS (= DSCP + ECN) information from the
    // incoming packet.
    //
    Option = TRUE;
    Result =
        setsockopt(
            SocketContext->SocketFd,
            IPPROTO_IPV6,
            IPV6_ECN,
            (const void*)&Option,
            sizeof(Option));
    if (Result == SOCKET_ERROR) {
        int WsaError = WSAGetLastError();
        QuicTraceEvent(
            DatapathErrorStatus,
            "[data][%p] ERROR, %u, %s.",
            Binding,
            WsaError,
            "setsockopt(IPV6_ECN) failed");
        Status = HRESULT_FROM_WIN32(WsaError);
        goto Exit;
    }

    Option = TRUE;
    Result =
        setsockopt(
            SocketContext->SocketFd,
            IPPROTO_IP,
            IP_ECN,
            (const void*)&Option,
            sizeof(Option));
    if (Result == SOCKET_ERROR) {
        int WsaError = WSAGetLastError();
        QuicTraceEvent(
            DatapathErrorStatus,
            "[data][%p] ERROR, %u, %s.",
            Binding,
            WsaError,
            "setsockopt(IP_ECN) failed");
        Status = HRESULT_FROM_WIN32(WsaError);
        goto Exit;
    }

    //
    // The socket is shared by multiple QUIC endpoints, so increase the receive
    // buffer size.
    //
    Option = INT32_MAX;
    Result =
        setsockopt(
            SocketContext->SocketFd,
            SOL_SOCKET,
            SO_RCVBUF,
            (const void*)&Option,
            sizeof(Option));
    if (Result == SOCKET_ERROR) {
        int WsaError = WSAGetLastError();
        QuicTraceEvent(
            DatapathErrorStatus,
            "[data][%p] ERROR, %u, %s.",
            Binding,
            WsaError,
            "setsockopt(SO_RCVBUF) failed");
        Status = HRESULT_FROM_WIN32(WsaError);
        goto Exit;
    }

#ifdef UDP_RECV_MAX_COALESCED_SIZE
        if (SocketContext->Binding->Datapath->Features & CXPLAT_DATAPATH_FEATURE_RECV_COALESCING) {
            Option = MAX_URO_PAYLOAD_LENGTH;
            Result =
                setsockopt(
                    SocketContext->SocketFd,
                    IPPROTO_UDP,
                    UDP_RECV_MAX_COALESCED_SIZE,
                    (char*)&Option,
                    sizeof(Option));
            if (Result == SOCKET_ERROR) {
                int WsaError = WSAGetLastError();
                QuicTraceEvent(
                    DatapathErrorStatus,
                    "[data][%p] ERROR, %u, %s.",
                    Binding,
                    WsaError,
                    "Set UDP_RECV_MAX_COALESCED_SIZE");
                Status = HRESULT_FROM_WIN32(WsaError);
                goto Exit;
            }
        }
#endif

    CxPlatCopyMemory(&MappedAddress, &Binding->LocalAddress, sizeof(MappedAddress));

    Result =
        bind(
            SocketContext->SocketFd,
            &MappedAddress,
            sizeof(MappedAddress));
    if (Result == SOCKET_ERROR) {
        int WsaError = WSAGetLastError();
        QuicTraceEvent(
            DatapathErrorStatus,
            "[data][%p] ERROR, %u, %s.",
            Binding,
            WsaError,
            "bind failed");
        Status = HRESULT_FROM_WIN32(WsaError);
        goto Exit;
    }

    if (RemoteAddress != NULL) {
        CxPlatZeroMemory(&MappedAddress, sizeof(MappedAddress));
        CxPlatConvertToMappedV6(RemoteAddress, &MappedAddress);

        if (MappedAddress.Ipv6.sin6_family == QUIC_ADDRESS_FAMILY_INET6) {
            MappedAddress.Ipv6.sin6_family = AF_INET6;
        }

        Result =
            connect(
                SocketContext->SocketFd,
                &MappedAddress,
                sizeof(MappedAddress));

        if (Result == SOCKET_ERROR) {
            int WsaError = WSAGetLastError();
            QuicTraceEvent(
                DatapathErrorStatus,
                "[data][%p] ERROR, %u, %s.",
                Binding,
                WsaError,
                "connect failed");
            Status = HRESULT_FROM_WIN32(WsaError);
            goto Exit;
        }
        Binding->Connected = TRUE;
    }

    //
    // If no specific local port was indicated, then the stack just
    // assigned this socket a port. We need to query it and use it for
    // all the other sockets we are going to create.
    //
    AssignedLocalAddressLength = sizeof(Binding->LocalAddress);
    Result =
        getsockname(
            SocketContext->SocketFd,
            (struct sockaddr *)&Binding->LocalAddress,
            &AssignedLocalAddressLength);
    if (Result == SOCKET_ERROR) {
        int WsaError = WSAGetLastError();
        QuicTraceEvent(
            DatapathErrorStatus,
            "[data][%p] ERROR, %u, %s.",
            Binding,
            WsaError,
            "getsockname failed");
        Status = HRESULT_FROM_WIN32(WsaError);
        goto Exit;
    }

#if DEBUG
    if (LocalAddress && LocalAddress->Ipv4.sin_port != 0) {
        CXPLAT_DBG_ASSERT(LocalAddress->Ipv4.sin_port == Binding->LocalAddress.Ipv4.sin_port);
    } else if (RemoteAddress && LocalAddress && LocalAddress->Ipv4.sin_port == 0) {
        //
        // A client socket being assigned the same port as a remote socket causes issues later
        // in the datapath and binding paths. Check to make sure this case was not given to us.
        //
        CXPLAT_DBG_ASSERT(Binding->LocalAddress.Ipv4.sin_port != RemoteAddress->Ipv4.sin_port);
    }
#else
    UNREFERENCED_PARAMETER(LocalAddress);
#endif

Exit:

    if (QUIC_FAILED(Status)) {
        closesocket(SocketContext->SocketFd);
        SocketContext->SocketFd = INVALID_SOCKET;
    }

    return Status;
}

void
CxPlatSocketContextUninitializeComplete(
    _In_ CXPLAT_SOCKET_CONTEXT* SocketContext
    )
{
    for (ssize_t i = 0; i < CXPLAT_MAX_BATCH_RECEIVE; i++) {
        if (SocketContext->CurrentRecvBlocks[i] != NULL) {
            CxPlatRecvDataReturn(&SocketContext->CurrentRecvBlocks[i]->RecvPacket);
        }
    }

    while (!CxPlatListIsEmpty(&SocketContext->PendingSendDataHead)) {
        CxPlatSendDataFree(
            CXPLAT_CONTAINING_RECORD(
                CxPlatListRemoveHead(&SocketContext->PendingSendDataHead),
                CXPLAT_SEND_DATA,
                PendingSendLinkage));
    }

    SOCK_NOTIFY_REGISTRATION Reg = {
        .socket = SocketContext->SocketFd,
        .completionKey = NULL,
        .eventFilter = SOCK_NOTIFY_REGISTER_EVENTS_ALL,
        .operation = SOCK_NOTIFY_OP_REMOVE,
        .triggerFlags = SOCK_NOTIFY_TRIGGER_LEVEL | SOCK_NOTIFY_TRIGGER_PERSISTENT,
        .registrationResult = 0
    };
    ProcessSocketNotifications(
        SocketContext->ProcContext->CompletionPort,
        1,
        &Reg,
        0,
        0,
        NULL,
        NULL);
    closesocket(SocketContext->SocketFd);

    CxPlatRundownRelease(&SocketContext->Binding->Rundown);
}

void
CxPlatSocketContextUninitialize(
    _In_ CXPLAT_SOCKET_CONTEXT* SocketContext
    )
{
    SOCK_NOTIFY_REGISTRATION Reg = {
        .socket = SocketContext->SocketFd,
        .completionKey = NULL,
        .eventFilter = SOCK_NOTIFY_REGISTER_EVENTS_ALL,
        .operation = SOCK_NOTIFY_OP_REMOVE,
        .triggerFlags = SOCK_NOTIFY_TRIGGER_LEVEL | SOCK_NOTIFY_TRIGGER_PERSISTENT,
        .registrationResult = 0
    };
    ProcessSocketNotifications(
        SocketContext->ProcContext->CompletionPort,
        1,
        &Reg,
        0,
        0,
        NULL,
        NULL);

    if (CxPlatCurThreadID() != SocketContext->ProcContext->ThreadId) {
        PostQueuedCompletionStatus(SocketContext->ProcContext->CompletionPort, 0, &SocketContext->EventContexts[QUIC_SOCK_EVENT_CLEANUP], NULL);
    } else {
        CxPlatSocketContextUninitializeComplete(SocketContext);
    }
}

QUIC_STATUS
CxPlatSocketContextPrepareReceive(
    _In_ CXPLAT_SOCKET_CONTEXT* SocketContext
    )
{
    CxPlatZeroMemory(&SocketContext->RecvMsgHdr, sizeof(SocketContext->RecvMsgHdr));
    CxPlatZeroMemory(&SocketContext->RecvMsgControl, sizeof(SocketContext->RecvMsgControl));

    for (ssize_t i = 0; i < CXPLAT_MAX_BATCH_RECEIVE; i++) {
        if (SocketContext->CurrentRecvBlocks[i] == NULL) {
            SocketContext->CurrentRecvBlocks[i] =
                CxPlatDataPathAllocRecvBlock(SocketContext->ProcContext);
            if (SocketContext->CurrentRecvBlocks[i] == NULL) {
                QuicTraceEvent(
                    AllocFailure,
                    "Allocation of '%s' failed. (%llu bytes)",
                    "CXPLAT_DATAPATH_RECV_BLOCK",
                    0);
                return QUIC_STATUS_OUT_OF_MEMORY;
            }
        }
        CXPLAT_DATAPATH_RECV_BLOCK* CurrentBlock = SocketContext->CurrentRecvBlocks[i];
        struct msghdr* MsgHdr = &SocketContext->RecvMsgHdr[i].msg_hdr;

        SocketContext->RecvIov[i].iov_base = CurrentBlock->RecvPacket.Buffer;
        CurrentBlock->RecvPacket.BufferLength = SocketContext->RecvIov[i].iov_len;
        CurrentBlock->RecvPacket.Route = &CurrentBlock->Route;

        MsgHdr->msg_name = &CurrentBlock->RecvPacket.Route->RemoteAddress;
        MsgHdr->msg_namelen = sizeof(CurrentBlock->RecvPacket.Route->RemoteAddress);
        MsgHdr->msg_iov = &SocketContext->RecvIov[i];
        MsgHdr->msg_iovlen = 1;
        MsgHdr->msg_control = &SocketContext->RecvMsgControl[i].Data;
        MsgHdr->msg_controllen = sizeof(SocketContext->RecvMsgControl[i].Data);
        MsgHdr->msg_flags = 0;
    }

    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS
CxPlatSocketContextStartReceive(
    _In_ CXPLAT_SOCKET_CONTEXT* SocketContext
    )
{
    QUIC_STATUS Status = CxPlatSocketContextPrepareReceive(SocketContext);
    if (QUIC_FAILED(Status)) {
        goto Error;
    }

    SOCK_NOTIFY_REGISTRATION reg = {
        .socket = SocketContext->SocketFd,
        .completionKey = &SocketContext->EventContexts[QUIC_SOCK_EVENT_SOCKET],
        .eventFilter = SOCK_NOTIFY_REGISTER_EVENT_IN | SOCK_NOTIFY_REGISTER_EVENT_HANGUP,
        .operation = SOCK_NOTIFY_OP_ENABLE,
        .triggerFlags = SOCK_NOTIFY_TRIGGER_LEVEL | SOCK_NOTIFY_TRIGGER_PERSISTENT,
        .registrationResult = 0
    };

    DWORD Ret = ProcessSocketNotifications(
        SocketContext->ProcContext->CompletionPort,
        1,
        &reg,
        0,
        0,
        NULL,
        NULL);
    if (Ret != ERROR_SUCCESS) {
        Status = HRESULT_FROM_WIN32(Ret);
        QuicTraceEvent(
            DatapathErrorStatus,
            "[data][%p] ERROR, %u, %s.",
            SocketContext->Binding,
            Status,
            "ProcessSocketNotifications failed");
        //
        // Return any allocations
        //
        for (int i = 0; i < CXPLAT_MAX_BATCH_RECEIVE; i++) {
            if (SocketContext->CurrentRecvBlocks[i] != NULL) {
                CxPlatRecvDataReturn(&SocketContext->CurrentRecvBlocks[i]->RecvPacket);
            }
        }

        goto Error;
    }

Error:

    return Status;
}

void
CxPlatSocketContextRecvComplete(
    _In_ CXPLAT_SOCKET_CONTEXT* SocketContext,
    _In_ int MessagesReceived
    )
{
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    uint32_t BytesTransferred = 0;

    CXPLAT_FRE_ASSERT(MessagesReceived <= CXPLAT_MAX_BATCH_RECEIVE);

    CXPLAT_RECV_DATA* DatagramHead = NULL;
    CXPLAT_RECV_DATA* DatagramTail = NULL;

    for (int CurrentMessage = 0; CurrentMessage < MessagesReceived; CurrentMessage++) {
        CXPLAT_DATAPATH_RECV_BLOCK* CurrentBlock = SocketContext->CurrentRecvBlocks[CurrentMessage];
        SocketContext->CurrentRecvBlocks[CurrentMessage] = NULL;
        CXPLAT_RECV_DATA* RecvPacket = &CurrentBlock->RecvPacket;

        if (DatagramHead == NULL) {
            DatagramHead = RecvPacket;
            DatagramTail = DatagramHead;
        } else {
            DatagramTail->Next = RecvPacket;
            DatagramTail = DatagramTail->Next;
        }

        BOOLEAN FoundLocalAddr = FALSE;
        BOOLEAN FoundTOS = FALSE;
        QUIC_ADDR* LocalAddr = &RecvPacket->Route->LocalAddress;
        if (LocalAddr->Ipv6.sin6_family == AF_INET6) {
            LocalAddr->Ipv6.sin6_family = QUIC_ADDRESS_FAMILY_INET6;
        }
        QUIC_ADDR* RemoteAddr = &RecvPacket->Route->RemoteAddress;
        if (RemoteAddr->Ipv6.sin6_family == AF_INET6) {
            RemoteAddr->Ipv6.sin6_family = QUIC_ADDRESS_FAMILY_INET6;
        }
        CxPlatConvertFromMappedV6(RemoteAddr, RemoteAddr);

        RecvPacket->BufferLength = SocketContext->RecvMsgHdr[CurrentMessage].msg_len;
        BytesTransferred += RecvPacket->BufferLength;

        RecvPacket->TypeOfService = 0;

        struct cmsghdr *CMsg;
        struct msghdr* Msg = &SocketContext->RecvMsgHdr[CurrentMessage].msg_hdr;
        for (CMsg = CMSG_FIRSTHDR(Msg);
            CMsg != NULL;
            CMsg = CMSG_NXTHDR(Msg, CMsg)) {

            if (CMsg->cmsg_level == IPPROTO_IPV6) {
                if (CMsg->cmsg_type == IPV6_PKTINFO) {
                    struct in6_pktinfo* PktInfo6 = (struct in6_pktinfo*) CMSG_DATA(CMsg);
                    LocalAddr->Ip.sa_family = QUIC_ADDRESS_FAMILY_INET6;
                    LocalAddr->Ipv6.sin6_addr = PktInfo6->ipi6_addr;
                    LocalAddr->Ipv6.sin6_port = SocketContext->Binding->LocalAddress.Ipv6.sin6_port;
                    CxPlatConvertFromMappedV6(LocalAddr, LocalAddr);

                    LocalAddr->Ipv6.sin6_scope_id = PktInfo6->ipi6_ifindex;
                    FoundLocalAddr = TRUE;
                } else if (CMsg->cmsg_type == IPV6_TCLASS) {
                    RecvPacket->TypeOfService = *(uint8_t *)CMSG_DATA(CMsg);
                    FoundTOS = TRUE;
                }
            } else if (CMsg->cmsg_level == IPPROTO_IP) {
                if (CMsg->cmsg_type == IP_PKTINFO) {
                    struct in_pktinfo* PktInfo = (struct in_pktinfo*)CMSG_DATA(CMsg);
                    LocalAddr->Ip.sa_family = QUIC_ADDRESS_FAMILY_INET;
                    LocalAddr->Ipv4.sin_addr = PktInfo->ipi_addr;
                    LocalAddr->Ipv4.sin_port = SocketContext->Binding->LocalAddress.Ipv6.sin6_port;
                    LocalAddr->Ipv6.sin6_scope_id = PktInfo->ipi_ifindex;
                    FoundLocalAddr = TRUE;
                } else if (CMsg->cmsg_type == IP_TOS) {
                    RecvPacket->TypeOfService = *(uint8_t *)CMSG_DATA(CMsg);
                    FoundTOS = TRUE;
                }
            }
        }

        CXPLAT_FRE_ASSERT(FoundLocalAddr);
        CXPLAT_FRE_ASSERT(FoundTOS);

        RecvPacket->PartitionIndex = SocketContext->ProcContext->Index;

        QuicTraceEvent(
            DatapathRecv,
            "[data][%p] Recv %u bytes (segment=%hu) Src=%!ADDR! Dst=%!ADDR!",
            SocketContext->Binding,
            (uint32_t)RecvPacket->BufferLength,
            (uint32_t)RecvPacket->BufferLength,
            CASTED_CLOG_BYTEARRAY(sizeof(*LocalAddr), LocalAddr),
            CASTED_CLOG_BYTEARRAY(sizeof(*RemoteAddr), RemoteAddr));
    }

    if (BytesTransferred == 0 || DatagramHead == NULL) {
        QuicTraceLogWarning(
            DatapathRecvEmpty,
            "[data][%p] Dropping datagram with empty payload.",
            SocketContext->Binding);
        goto Drop;
    }

    if (!SocketContext->Binding->PcpBinding) {
        CXPLAT_DBG_ASSERT(SocketContext->Binding->Datapath->UdpHandlers.Receive);
        SocketContext->Binding->Datapath->UdpHandlers.Receive(
            SocketContext->Binding,
            SocketContext->Binding->ClientContext,
            DatagramHead);
    } else{
        CxPlatPcpRecvCallback(
            SocketContext->Binding,
            SocketContext->Binding->ClientContext,
            DatagramHead);
    }

Drop: ;

    int32_t RetryCount = 0;
    do {
        Status = CxPlatSocketContextPrepareReceive(SocketContext);
    } while (!QUIC_SUCCEEDED(Status) && ++RetryCount < 10);

    if (!QUIC_SUCCEEDED(Status)) {
        CXPLAT_DBG_ASSERT(Status == QUIC_STATUS_OUT_OF_MEMORY);
        QuicTraceEvent(
            DatapathErrorStatus,
            "[data][%p] ERROR, %u, %s.",
            SocketContext->Binding,
            Status,
            "CxPlatSocketContextPrepareReceive failed multiple times. Receive will no longer work.");
    }
}

//
// N.B Requires SocketContext->PendingSendDataLock to be locked.
//
void
CxPlatSocketContextPendSend(
    _In_ CXPLAT_SOCKET_CONTEXT* SocketContext,
    _In_ CXPLAT_SEND_DATA* SendData,
    _In_opt_ const QUIC_ADDR* LocalAddress,
    _In_ const QUIC_ADDR* RemoteAddress
    )
{
    if (LocalAddress != NULL) {
        CxPlatCopyMemory(
            &SendData->LocalAddress,
            LocalAddress,
            sizeof(*LocalAddress));
        SendData->Bind = TRUE;
    }

    CxPlatCopyMemory(
        &SendData->RemoteAddress,
        RemoteAddress,
        sizeof(*RemoteAddress));

    //
    // This is a new send that wasn't previously pended. Add it to the end
    // of the queue.
    //
    CxPlatListInsertTail(
        &SocketContext->PendingSendDataHead,
        &SendData->PendingSendLinkage);
}

QUIC_STATUS
CxPlatSocketContextSendComplete(
    _In_ CXPLAT_SOCKET_CONTEXT* SocketContext
    )
{
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    CXPLAT_SEND_DATA* SendData = NULL;

    SOCK_NOTIFY_REGISTRATION reg = {
        .socket = SocketContext->SocketFd,
        .completionKey = &SocketContext->EventContexts[QUIC_SOCK_EVENT_SOCKET],
        .eventFilter = SOCK_NOTIFY_REGISTER_EVENT_IN | SOCK_NOTIFY_REGISTER_EVENT_HANGUP,
        .operation = SOCK_NOTIFY_OP_ENABLE,
        .triggerFlags = SOCK_NOTIFY_TRIGGER_LEVEL | SOCK_NOTIFY_TRIGGER_PERSISTENT,
        .registrationResult = 0
    };

    DWORD Ret = ProcessSocketNotifications(
        SocketContext->ProcContext->CompletionPort,
        1,
        &reg,
        0,
        0,
        NULL,
        NULL);
    if (Ret != ERROR_SUCCESS) {
        Status = HRESULT_FROM_WIN32(Ret);
        QuicTraceEvent(
            DatapathErrorStatus,
            "[data][%p] ERROR, %u, %s.",
            SocketContext->Binding,
            Status,
            "ProcessSocketNotifications failed");
        return Status;
    }

    CxPlatLockAcquire(&SocketContext->PendingSendDataLock);
    if (!CxPlatListIsEmpty(&SocketContext->PendingSendDataHead)) {
        SendData =
            CXPLAT_CONTAINING_RECORD(
                SocketContext->PendingSendDataHead.Flink,
                CXPLAT_SEND_DATA,
                PendingSendLinkage);
    }
    CxPlatLockRelease(&SocketContext->PendingSendDataLock);
    if (SendData == NULL) {
        return Status;
    }

    do {
        Status =
            CxPlatSocketSendInternal(
                SocketContext->Binding,
                SendData->Bind ? &SendData->LocalAddress : NULL,
                &SendData->RemoteAddress,
                SendData,
                TRUE);
        CxPlatLockAcquire(&SocketContext->PendingSendDataLock);
        if (Status != QUIC_STATUS_PENDING) {
            CxPlatListRemoveHead(&SocketContext->PendingSendDataHead);
            CxPlatSendDataFree(SendData);
            if (!CxPlatListIsEmpty(&SocketContext->PendingSendDataHead)) {
                SendData =
                    CXPLAT_CONTAINING_RECORD(
                        SocketContext->PendingSendDataHead.Flink,
                        CXPLAT_SEND_DATA,
                        PendingSendLinkage);
            } else {
                SendData = NULL;
            }
        }
        CxPlatLockRelease(&SocketContext->PendingSendDataLock);
    } while (Status == QUIC_STATUS_SUCCESS && SendData != NULL);

    return Status;
}

void
CxPlatSocketContextProcessEvents(
    _In_ void* EventPtr,
    _In_ UINT32 Events
    )
{
    uint8_t EventType = *(uint8_t*)EventPtr;
    CXPLAT_SOCKET_CONTEXT* SocketContext =
        (CXPLAT_SOCKET_CONTEXT*)(
            (uint8_t*)CXPLAT_CONTAINING_RECORD(EventPtr, CXPLAT_SOCKET_CONTEXT, EventContexts) -
            EventType);

    if (EventType == QUIC_SOCK_EVENT_CLEANUP) {
        CXPLAT_DBG_ASSERT(SocketContext->Binding->Shutdown);
        CxPlatSocketContextUninitializeComplete(SocketContext);
        return;
    }

    CXPLAT_DBG_ASSERT(EventType == QUIC_SOCK_EVENT_SOCKET);

    if (SOCK_NOTIFY_EVENT_ERR & Events) {
        int ErrNum = 0;
        socklen_t OptLen = sizeof(ErrNum);
        int Ret =
            getsockopt(
                SocketContext->SocketFd,
                SOL_SOCKET,
                SO_ERROR,
                &ErrNum,
                &OptLen);
        if (Ret < 0) {
            QuicTraceEvent(
                DatapathErrorStatus,
                "[data][%p] ERROR, %u, %s.",
                SocketContext->Binding,
                errno,
                "getsockopt(SO_ERROR) failed");
        } else {
            QuicTraceEvent(
                DatapathErrorStatus,
                "[data][%p] ERROR, %u, %s.",
                SocketContext->Binding,
                ErrNum,
                "Socket error event");

            //
            // Send unreachable notification to MsQuic if any related
            // errors were received.
            //
            if (ErrNum == ECONNREFUSED ||
                ErrNum == EHOSTUNREACH ||
                ErrNum == ENETUNREACH) {
                if (!SocketContext->Binding->PcpBinding) {
                    SocketContext->Binding->Datapath->UdpHandlers.Unreachable(
                        SocketContext->Binding,
                        SocketContext->Binding->ClientContext,
                        &SocketContext->Binding->RemoteAddress);
                }
            }
        }
    }

    if (SOCK_NOTIFY_EVENT_IN & Events) {
        while (TRUE) {

            // for (ssize_t i = 0; i < CXPLAT_MAX_BATCH_RECEIVE; i++) {
            //     CXPLAT_DBG_ASSERT(SocketContext->CurrentRecvBlocks[i] != NULL);
            // }

            CXPLAT_DATAPATH_RECV_BLOCK* RecvBlock =
                CxPlatDataPathAllocRecvBlock(SocketContext->ProcContext);

            if (RecvBlock == NULL) {
                // TODO Log
                break;
            }




            SocketContext->Binding->Datapath->WSARecvMsg(
                SocketContext->SocketFd,

            )

            int Ret =
                recvmmsg(
                    SocketContext->SocketFd,
                    SocketContext->RecvMsgHdr,
                    CXPLAT_MAX_BATCH_RECEIVE,
                    0,
                    NULL);
            if (Ret < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    QuicTraceEvent(
                        DatapathErrorStatus,
                        "[data][%p] ERROR, %u, %s.",
                        SocketContext->Binding,
                        errno,
                        "recvmmsg failed");
                }
                break;
            }
            CxPlatSocketContextRecvComplete(SocketContext, Ret);
        }
    }

    if (SOCK_NOTIFY_EVENT_OUT & Events) {
        CxPlatSocketContextSendComplete(SocketContext);
    }
}

//
// Datapath binding interface.
//

QUIC_STATUS
CxPlatSocketCreateUdp(
    _In_ CXPLAT_DATAPATH* Datapath,
    _In_ const CXPLAT_UDP_CONFIG* Config,
    _Out_ CXPLAT_SOCKET** NewBinding
    )
{
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    BOOLEAN IsServerSocket = Config->RemoteAddress == NULL;
    int32_t SuccessfulStartReceives = -1;

    CXPLAT_DBG_ASSERT(Datapath->UdpHandlers.Receive != NULL || Config->Flags & CXPLAT_SOCKET_FLAG_PCP);

    uint32_t SocketCount = IsServerSocket ? Datapath->ProcCount : 1;
    uint32_t CurrentProc = CxPlatProcCurrentNumber() % Datapath->ProcCount;
    CXPLAT_FRE_ASSERT(SocketCount > 0);
    size_t BindingLength =
        sizeof(CXPLAT_SOCKET) +
        SocketCount * sizeof(CXPLAT_SOCKET_CONTEXT);

    CXPLAT_SOCKET* Binding =
        (CXPLAT_SOCKET*)CXPLAT_ALLOC_PAGED(BindingLength, QUIC_POOL_SOCKET);
    if (Binding == NULL) {
        Status = QUIC_STATUS_OUT_OF_MEMORY;
        QuicTraceEvent(
            AllocFailure,
            "Allocation of '%s' failed. (%llu bytes)",
            "CXPLAT_SOCKET",
            BindingLength);
        goto Exit;
    }

    QuicTraceEvent(
        DatapathCreated,
        "[data][%p] Created, local=%!ADDR!, remote=%!ADDR!",
        Binding,
        CASTED_CLOG_BYTEARRAY(Config->LocalAddress ? sizeof(*Config->LocalAddress) : 0, Config->LocalAddress),
        CASTED_CLOG_BYTEARRAY(Config->RemoteAddress ? sizeof(*Config->RemoteAddress) : 0, Config->RemoteAddress));

    CxPlatZeroMemory(Binding, BindingLength);
    Binding->Datapath = Datapath;
    Binding->ClientContext = Config->CallbackContext;
    Binding->HasFixedRemoteAddress = (Config->RemoteAddress != NULL);
    Binding->Mtu = CXPLAT_MAX_MTU;
    CxPlatRundownInitialize(&Binding->Rundown);
    if (Config->LocalAddress) {
        CxPlatConvertToMappedV6(Config->LocalAddress, &Binding->LocalAddress);
    } else {
        Binding->LocalAddress.Ip.sa_family = QUIC_ADDRESS_FAMILY_INET6;
    }
    for (uint32_t i = 0; i < SocketCount; i++) {
        Binding->SocketContexts[i].Binding = Binding;
        Binding->SocketContexts[i].SocketFd = INVALID_SOCKET;
        for (ssize_t j = 0; j < CXPLAT_MAX_BATCH_RECEIVE; j++) {
            Binding->SocketContexts[i].RecvIov[j].iov_len =
                Binding->Mtu - CXPLAT_MIN_IPV4_HEADER_SIZE - CXPLAT_UDP_HEADER_SIZE;
        }
        Binding->SocketContexts[i].ProcContext = &Datapath->ProcContexts[IsServerSocket ? i : CurrentProc];
        CxPlatListInitializeHead(&Binding->SocketContexts[i].PendingSendDataHead);
        CxPlatLockInitialize(&Binding->SocketContexts[i].PendingSendDataLock);
        CxPlatRundownAcquire(&Binding->Rundown);
    }

    CxPlatRundownAcquire(&Datapath->BindingsRundown);
    if (Config->Flags & CXPLAT_SOCKET_FLAG_PCP) {
        Binding->PcpBinding = TRUE;
    }

    for (uint32_t i = 0; i < SocketCount; i++) {
        Status =
            CxPlatSocketContextInitialize(
                &Binding->SocketContexts[i],
                Config->LocalAddress,
                Config->RemoteAddress,
                Config->Flags & CXPLAT_SOCKET_FLAG_SHARE);
        if (QUIC_FAILED(Status)) {
            goto Exit;
        }
    }

    if (IsServerSocket) {
        //
        // The return value is being ignored here, as if a system does not support
        // bpf we still want the server to work. If this happens, the sockets will
        // round robin, but each flow will be sent to the same socket, just not
        // based on RSS.
        //
        (void)CxPlatSocketConfigureRss(&Binding->SocketContexts[0], SocketCount);
    }

    CxPlatConvertFromMappedV6(&Binding->LocalAddress, &Binding->LocalAddress);
    Binding->LocalAddress.Ipv6.sin6_scope_id = 0;

    if (Config->RemoteAddress != NULL) {
        Binding->RemoteAddress = *Config->RemoteAddress;
    } else {
        Binding->RemoteAddress.Ipv4.sin_port = 0;
    }

    //
    // Must set output pointer before starting receive path, as the receive path
    // will try to use the output.
    //
    *NewBinding = Binding;

    SuccessfulStartReceives = 0;
    for (uint32_t i = 0; i < SocketCount; i++) {
        Status =
            CxPlatSocketContextStartReceive(
                &Binding->SocketContexts[i]);
        if (QUIC_FAILED(Status)) {
            SuccessfulStartReceives = (int32_t)i;
            goto Exit;
        }
    }

    Status = QUIC_STATUS_SUCCESS;

Exit:

    if (QUIC_FAILED(Status)) {
        if (Binding != NULL) {
            QuicTraceEvent(
                DatapathDestroyed,
                "[data][%p] Destroyed",
                Binding);

            if (SuccessfulStartReceives >= 0) {
                uint32_t CurrentSocket = 0;
                Binding->Shutdown = TRUE;

                //
                // First shutdown any sockets that fully started
                //
                for (; CurrentSocket < (uint32_t)SuccessfulStartReceives; CurrentSocket++) {
                    CxPlatSocketContextUninitialize(&Binding->SocketContexts[CurrentSocket]);
                }
                //
                // Then shutdown any sockets that failed to start
                //
                for (; CurrentSocket < SocketCount; CurrentSocket++) {
                    CxPlatSocketContextUninitializeComplete(&Binding->SocketContexts[CurrentSocket]);
                }
            } else {
                //
                // No sockets fully started. Only uninitialize static things
                //
                for (uint32_t i = 0; i < SocketCount; i++) {
                    CXPLAT_SOCKET_CONTEXT* SocketContext = &Binding->SocketContexts[i];
                    HANDLE IOCP = SocketContext->ProcContext->CompletionPort;


                    if (SocketContext->SocketFd != INVALID_SOCKET) {
                        SOCK_NOTIFY_REGISTRATION Reg = {
                            .socket = SocketContext->SocketFd,
                            .completionKey = NULL,
                            .eventFilter = SOCK_NOTIFY_REGISTER_EVENTS_ALL,
                            .operation = SOCK_NOTIFY_OP_REMOVE,
                            .triggerFlags = SOCK_NOTIFY_TRIGGER_LEVEL | SOCK_NOTIFY_TRIGGER_PERSISTENT,
                            .registrationResult = 0
                        };
                        ProcessSocketNotifications(
                            SocketContext->ProcContext->CompletionPort,
                            1,
                            &Reg,
                            0,
                            0,
                            NULL,
                            NULL);
                        closesocket(SocketContext->SocketFd);
                    }
                    CxPlatRundownRelease(&Binding->Rundown);
                }
            }
            CxPlatRundownReleaseAndWait(&Binding->Rundown);
            CxPlatRundownRelease(&Datapath->BindingsRundown);
            CxPlatRundownUninitialize(&Binding->Rundown);
            for (uint32_t i = 0; i < SocketCount; i++) {
                CxPlatLockUninitialize(&Binding->SocketContexts[i].PendingSendDataLock);
            }
            CXPLAT_FREE(Binding, QUIC_POOL_SOCKET);
            Binding = NULL;
        }
    }

    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
CxPlatSocketCreateTcp(
    _In_ CXPLAT_DATAPATH* Datapath,
    _In_opt_ const QUIC_ADDR* LocalAddress,
    _In_ const QUIC_ADDR* RemoteAddress,
    _In_opt_ void* CallbackContext,
    _Out_ CXPLAT_SOCKET** Socket
    )
{
    UNREFERENCED_PARAMETER(Datapath);
    UNREFERENCED_PARAMETER(LocalAddress);
    UNREFERENCED_PARAMETER(RemoteAddress);
    UNREFERENCED_PARAMETER(CallbackContext);
    UNREFERENCED_PARAMETER(Socket);
    return QUIC_STATUS_NOT_SUPPORTED;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
QUIC_STATUS
CxPlatSocketCreateTcpListener(
    _In_ CXPLAT_DATAPATH* Datapath,
    _In_opt_ const QUIC_ADDR* LocalAddress,
    _In_opt_ void* CallbackContext,
    _Out_ CXPLAT_SOCKET** Socket
    )
{
    UNREFERENCED_PARAMETER(Datapath);
    UNREFERENCED_PARAMETER(LocalAddress);
    UNREFERENCED_PARAMETER(CallbackContext);
    UNREFERENCED_PARAMETER(Socket);
    return QUIC_STATUS_NOT_SUPPORTED;
}

void
CxPlatSocketDelete(
    _Inout_ CXPLAT_SOCKET* Socket
    )
{
    CXPLAT_DBG_ASSERT(Socket != NULL);
    QuicTraceEvent(
        DatapathDestroyed,
        "[data][%p] Destroyed",
        Socket);

    //
    // The function is called by the upper layer when it is completely done
    // with the UDP binding. It expects that after this call returns there will
    // be no additional upcalls related to this binding, and all outstanding
    // upcalls on different threads will be completed.
    //

    Socket->Shutdown = TRUE;
    uint32_t SocketCount = Socket->HasFixedRemoteAddress ? 1 : Socket->Datapath->ProcCount;
    for (uint32_t i = 0; i < SocketCount; ++i) {
        CxPlatSocketContextUninitialize(&Socket->SocketContexts[i]);
    }

    CxPlatRundownReleaseAndWait(&Socket->Rundown);
    CxPlatRundownRelease(&Socket->Datapath->BindingsRundown);

    CxPlatRundownUninitialize(&Socket->Rundown);
    for (uint32_t i = 0; i < SocketCount; i++) {
        CxPlatLockUninitialize(&Socket->SocketContexts[i].PendingSendDataLock);
    }
    CXPLAT_FREE(Socket, QUIC_POOL_SOCKET);
}

void
CxPlatSocketGetLocalAddress(
    _In_ CXPLAT_SOCKET* Socket,
    _Out_ QUIC_ADDR* Address
    )
{
    CXPLAT_DBG_ASSERT(Socket != NULL);
    *Address = Socket->LocalAddress;
}

void
CxPlatSocketGetRemoteAddress(
    _In_ CXPLAT_SOCKET* Socket,
    _Out_ QUIC_ADDR* Address
    )
{
    CXPLAT_DBG_ASSERT(Socket != NULL);
    *Address = Socket->RemoteAddress;
}

QUIC_STATUS
CxPlatSocketSetParam(
    _In_ CXPLAT_SOCKET* Socket,
    _In_ uint32_t Param,
    _In_ uint32_t BufferLength,
    _In_reads_bytes_(BufferLength) const uint8_t * Buffer
    )
{
    UNREFERENCED_PARAMETER(Socket);
    UNREFERENCED_PARAMETER(Param);
    UNREFERENCED_PARAMETER(BufferLength);
    UNREFERENCED_PARAMETER(Buffer);
    return QUIC_STATUS_NOT_SUPPORTED;
}

QUIC_STATUS
CxPlatSocketGetParam(
    _In_ CXPLAT_SOCKET* Socket,
    _In_ uint32_t Param,
    _Inout_ uint32_t* BufferLength,
    _Out_writes_bytes_opt_(*BufferLength) uint8_t * Buffer
    )
{
    UNREFERENCED_PARAMETER(Socket);
    UNREFERENCED_PARAMETER(Param);
    UNREFERENCED_PARAMETER(BufferLength);
    UNREFERENCED_PARAMETER(Buffer);
    return QUIC_STATUS_NOT_SUPPORTED;
}

CXPLAT_RECV_DATA*
CxPlatDataPathRecvPacketToRecvData(
    _In_ const CXPLAT_RECV_PACKET* const Packet
    )
{
    CXPLAT_DATAPATH_RECV_BLOCK* RecvBlock =
        (CXPLAT_DATAPATH_RECV_BLOCK*)
            ((char *)Packet - sizeof(CXPLAT_DATAPATH_RECV_BLOCK));

    return &RecvBlock->RecvPacket;
}

CXPLAT_RECV_PACKET*
CxPlatDataPathRecvDataToRecvPacket(
    _In_ const CXPLAT_RECV_DATA* const RecvData
    )
{
    CXPLAT_DATAPATH_RECV_BLOCK* RecvBlock =
        CXPLAT_CONTAINING_RECORD(RecvData, CXPLAT_DATAPATH_RECV_BLOCK, RecvPacket);

    return (CXPLAT_RECV_PACKET*)(RecvBlock + 1);
}

void
CxPlatRecvDataReturn(
    _In_opt_ CXPLAT_RECV_DATA* RecvDataChain
    )
{
    CXPLAT_RECV_DATA* Datagram;
    while ((Datagram = RecvDataChain) != NULL) {
        RecvDataChain = RecvDataChain->Next;
        CXPLAT_DATAPATH_RECV_BLOCK* RecvBlock =
            CXPLAT_CONTAINING_RECORD(Datagram, CXPLAT_DATAPATH_RECV_BLOCK, RecvPacket);
        CxPlatPoolFree(RecvBlock->OwningPool, RecvBlock);
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Success_(return != NULL)
CXPLAT_SEND_DATA*
CxPlatSendDataAlloc(
    _In_ CXPLAT_SOCKET* Socket,
    _In_ CXPLAT_ECN_TYPE ECN,
    _In_ uint16_t MaxPacketSize,
    _Inout_ CXPLAT_ROUTE* Route
    )
{
    UNREFERENCED_PARAMETER(Route);
    CXPLAT_DBG_ASSERT(Socket != NULL);

    CXPLAT_DATAPATH_PROC_CONTEXT* DatapathProc =
        &Socket->Datapath->ProcContexts[CxPlatProcCurrentNumber() % Socket->Datapath->ProcCount];

    CXPLAT_SEND_DATA* SendData =
        CxPlatPoolAlloc(&DatapathProc->SendDataPool);

    if (SendData == NULL) {
        QuicTraceEvent(
            AllocFailure,
            "Allocation of '%s' failed. (%llu bytes)",
            "CXPLAT_SEND_DATA",
            0);
        goto Exit;
    }

    CxPlatZeroMemory(SendData, sizeof(*SendData));

    SendData->Owner = DatapathProc;
    SendData->ECN = ECN;
    SendData->SegmentSize =
        (Socket->Datapath->Features & CXPLAT_DATAPATH_FEATURE_SEND_SEGMENTATION)
            ? MaxPacketSize : 0;

Exit:
    return SendData;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
CxPlatSendDataFree(
    _In_ CXPLAT_SEND_DATA* SendData
    )
{
    CXPLAT_DATAPATH_PROC_CONTEXT* DatapathProc = SendData->Owner;
    CXPLAT_POOL* BufferPool =
        SendData->SegmentSize > 0 ?
            &DatapathProc->LargeSendBufferPool : &DatapathProc->SendBufferPool;

    for (size_t i = 0; i < SendData->BufferCount; ++i) {
        CxPlatPoolFree(BufferPool, SendData->Buffers[i].Buffer);
    }

    CxPlatPoolFree(&DatapathProc->SendDataPool, SendData);
}

static
BOOLEAN
CxPlatSendDataCanAllocSendSegment(
    _In_ CXPLAT_SEND_DATA* SendData,
    _In_ uint16_t MaxBufferLength
    )
{
    if (!SendData->ClientBuffer.Buffer) {
        return FALSE;
    }

    CXPLAT_DBG_ASSERT(SendData->SegmentSize > 0);
    CXPLAT_DBG_ASSERT(SendData->BufferCount > 0);

    uint64_t BytesAvailable =
        CXPLAT_LARGE_SEND_BUFFER_SIZE -
            SendData->Buffers[SendData->BufferCount - 1].Length -
            SendData->ClientBuffer.Length;

    return MaxBufferLength <= BytesAvailable;
}

static
BOOLEAN
CxPlatSendDataCanAllocSend(
    _In_ CXPLAT_SEND_DATA* SendData,
    _In_ uint16_t MaxBufferLength
    )
{
    return
        (SendData->BufferCount < SendData->Owner->Datapath->MaxSendBatchSize) ||
        ((SendData->SegmentSize > 0) &&
            CxPlatSendDataCanAllocSendSegment(SendData, MaxBufferLength));
}

static
void
CxPlatSendDataFinalizeSendBuffer(
    _In_ CXPLAT_SEND_DATA* SendData
    )
{
    if (SendData->ClientBuffer.Length == 0) {
        //
        // There is no buffer segment outstanding at the client.
        //
        if (SendData->BufferCount > 0) {
            CXPLAT_DBG_ASSERT(SendData->Buffers[SendData->BufferCount - 1].Length < UINT16_MAX);
            SendData->TotalSize +=
                SendData->Buffers[SendData->BufferCount - 1].Length;
        }
        return;
    }

    CXPLAT_DBG_ASSERT(SendData->SegmentSize > 0 && SendData->BufferCount > 0);
    CXPLAT_DBG_ASSERT(SendData->ClientBuffer.Length > 0 && SendData->ClientBuffer.Length <= SendData->SegmentSize);
    CXPLAT_DBG_ASSERT(CxPlatSendDataCanAllocSendSegment(SendData, 0));

    //
    // Append the client's buffer segment to our internal send buffer.
    //
    SendData->Buffers[SendData->BufferCount - 1].Length +=
        SendData->ClientBuffer.Length;
    SendData->TotalSize += SendData->ClientBuffer.Length;

    if (SendData->ClientBuffer.Length == SendData->SegmentSize) {
        SendData->ClientBuffer.Buffer += SendData->SegmentSize;
        SendData->ClientBuffer.Length = 0;
    } else {
        //
        // The next segment allocation must create a new backing buffer.
        //
        SendData->ClientBuffer.Buffer = NULL;
        SendData->ClientBuffer.Length = 0;
    }
}

_Success_(return != NULL)
static
QUIC_BUFFER*
CxPlatSendDataAllocDataBuffer(
    _In_ CXPLAT_SEND_DATA* SendData,
    _In_ CXPLAT_POOL* BufferPool
    )
{
    CXPLAT_DBG_ASSERT(SendData->BufferCount < SendData->Owner->Datapath->MaxSendBatchSize);

    QUIC_BUFFER* Buffer = &SendData->Buffers[SendData->BufferCount];
    Buffer->Buffer = CxPlatPoolAlloc(BufferPool);
    if (Buffer->Buffer == NULL) {
        return NULL;
    }
    ++SendData->BufferCount;

    return Buffer;
}

_Success_(return != NULL)
static
QUIC_BUFFER*
CxPlatSendDataAllocPacketBuffer(
    _In_ CXPLAT_SEND_DATA* SendData,
    _In_ uint16_t MaxBufferLength
    )
{
    QUIC_BUFFER* Buffer =
        CxPlatSendDataAllocDataBuffer(SendData, &SendData->Owner->SendBufferPool);
    if (Buffer != NULL) {
        Buffer->Length = MaxBufferLength;
    }
    return Buffer;
}

_Success_(return != NULL)
static
QUIC_BUFFER*
CxPlatSendDataAllocSegmentBuffer(
    _In_ CXPLAT_SEND_DATA* SendData,
    _In_ uint16_t MaxBufferLength
    )
{
    CXPLAT_DBG_ASSERT(SendData->SegmentSize > 0);
    CXPLAT_DBG_ASSERT(MaxBufferLength <= SendData->SegmentSize);

    if (CxPlatSendDataCanAllocSendSegment(SendData, MaxBufferLength)) {

        //
        // All clear to return the next segment of our contiguous buffer.
        //
        SendData->ClientBuffer.Length = MaxBufferLength;
        return &SendData->ClientBuffer;
    }

    QUIC_BUFFER* Buffer = CxPlatSendDataAllocDataBuffer(SendData, &SendData->Owner->LargeSendBufferPool);
    if (Buffer == NULL) {
        return NULL;
    }

    //
    // Provide a virtual QUIC_BUFFER to the client. Once the client has committed
    // to a final send size, we'll append it to our internal backing buffer.
    //
    Buffer->Length = 0;
    SendData->ClientBuffer.Buffer = Buffer->Buffer;
    SendData->ClientBuffer.Length = MaxBufferLength;

    return &SendData->ClientBuffer;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Success_(return != NULL)
QUIC_BUFFER*
CxPlatSendDataAllocBuffer(
    _In_ CXPLAT_SEND_DATA* SendData,
    _In_ uint16_t MaxBufferLength
    )
{
    CXPLAT_DBG_ASSERT(SendData != NULL);
    CXPLAT_DBG_ASSERT(MaxBufferLength > 0);
    //CXPLAT_DBG_ASSERT(MaxBufferLength <= CXPLAT_MAX_MTU - CXPLAT_MIN_IPV4_HEADER_SIZE - CXPLAT_UDP_HEADER_SIZE);

    CxPlatSendDataFinalizeSendBuffer(SendData);

    if (!CxPlatSendDataCanAllocSend(SendData, MaxBufferLength)) {
        return NULL;
    }

    if (SendData->SegmentSize == 0) {
        return CxPlatSendDataAllocPacketBuffer(SendData, MaxBufferLength);
    }
    return CxPlatSendDataAllocSegmentBuffer(SendData, MaxBufferLength);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
CxPlatSendDataFreeBuffer(
    _In_ CXPLAT_SEND_DATA* SendData,
    _In_ QUIC_BUFFER* Buffer
    )
{
    //
    // This must be the final send buffer; intermediate buffers cannot be freed.
    //
    CXPLAT_DATAPATH_PROC_CONTEXT* DatapathProc = SendData->Owner;
#ifdef DEBUG
    uint8_t* TailBuffer = SendData->Buffers[SendData->BufferCount - 1].Buffer;
#endif

    if (SendData->SegmentSize == 0) {
#ifdef DEBUG
        CXPLAT_DBG_ASSERT(Buffer->Buffer == (uint8_t*)TailBuffer);
#endif

        CxPlatPoolFree(&DatapathProc->SendBufferPool, Buffer->Buffer);
        --SendData->BufferCount;
    } else {
#ifdef DEBUG
        TailBuffer += SendData->Buffers[SendData->BufferCount - 1].Length;
        CXPLAT_DBG_ASSERT(Buffer->Buffer == (uint8_t*)TailBuffer);
#endif

        if (SendData->Buffers[SendData->BufferCount - 1].Length == 0) {
            CxPlatPoolFree(&DatapathProc->LargeSendBufferPool, Buffer->Buffer);
            --SendData->BufferCount;
        }

        SendData->ClientBuffer.Buffer = NULL;
        SendData->ClientBuffer.Length = 0;
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
CxPlatSendDataIsFull(
    _In_ CXPLAT_SEND_DATA* SendData
    )
{
    return !CxPlatSendDataCanAllocSend(SendData, SendData->SegmentSize);
}

void
CxPlatSendDataComplete(
    _In_ CXPLAT_SOCKET_CONTEXT* SocketProc,
    _In_ CXPLAT_SEND_DATA* SendData,
    _In_ uint64_t IoResult
    )
{
    if (IoResult != QUIC_STATUS_SUCCESS) {
        QuicTraceEvent(
            DatapathErrorStatus,
            "[data][%p] ERROR, %u, %s.",
            SocketProc->Binding,
            IoResult,
            "sendmmsg completion");
    }

    // TODO to add TCP
    // if (SocketProc->Parent->Type != CXPLAT_SOCKET_UDP) {
    //     SocketProc->Parent->Datapath->TcpHandlers.SendComplete(
    //         SocketProc->Parent,
    //         SocketProc->Parent->ClientContext,
    //         IoResult,
    //         SendData->TotalSize);
    // }

    CxPlatSendDataFree(SendData);
}

QUIC_STATUS
CxPlatSocketSendInternal(
    _In_ CXPLAT_SOCKET* Socket,
    _In_ const QUIC_ADDR* LocalAddress,
    _In_ const QUIC_ADDR* RemoteAddress,
    _In_ CXPLAT_SEND_DATA* SendData,
    _In_ BOOLEAN IsPendedSend
    )
{
    QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
    CXPLAT_SOCKET_CONTEXT* SocketContext = NULL;
    QUIC_ADDR MappedRemoteAddress = {0};
    struct cmsghdr *CMsg = NULL;
    struct in_pktinfo *PktInfo = NULL;
    struct in6_pktinfo *PktInfo6 = NULL;
    BOOLEAN SendPending = FALSE;
    size_t TotalMessagesCount;

    CXPLAT_DBG_ASSERT(Socket != NULL && RemoteAddress != NULL && SendData != NULL);

    CXPLAT_STATIC_ASSERT(
        CMSG_SPACE(sizeof(struct in6_pktinfo)) >= CMSG_SPACE(sizeof(struct in_pktinfo)),
        "sizeof(struct in6_pktinfo) >= sizeof(struct in_pktinfo) failed");
    char ControlBuffer[
        CMSG_SPACE(sizeof(struct in6_pktinfo)) +
        CMSG_SPACE(sizeof(int))
    #ifdef UDP_SEGMENT
        + CMSG_SPACE(sizeof(uint16_t))
    #endif
        ] = {0};

    if (Socket->HasFixedRemoteAddress) {
        SocketContext = &Socket->SocketContexts[0];
    } else {
        uint32_t ProcNumber = CxPlatProcCurrentNumber() % Socket->Datapath->ProcCount;
        SocketContext = &Socket->SocketContexts[ProcNumber];
    }

    if (!IsPendedSend) {
        CxPlatSendDataFinalizeSendBuffer(SendData);
        QuicTraceEvent(
            DatapathSend,
            "[data][%p] Send %u bytes in %hhu buffers (segment=%hu) Dst=%!ADDR!, Src=%!ADDR!",
            Socket,
            SendData->TotalSize,
            SendData->BufferCount,
            SendData->SegmentSize,
            CASTED_CLOG_BYTEARRAY(sizeof(*RemoteAddress), RemoteAddress),
            CASTED_CLOG_BYTEARRAY(sizeof(*LocalAddress), LocalAddress));

        //
        // Check to see if we need to pend.
        //
        CxPlatLockAcquire(&SocketContext->PendingSendDataLock);
        if (!CxPlatListIsEmpty(&SocketContext->PendingSendDataHead)) {
            CxPlatSocketContextPendSend(
                SocketContext,
                SendData,
                LocalAddress,
                RemoteAddress);
            SendPending = TRUE;
        }
        CxPlatLockRelease(&SocketContext->PendingSendDataLock);
        if (SendPending) {
            Status = QUIC_STATUS_PENDING;
            goto Exit;
        }
    }

    //
    // Map V4 address to dual-stack socket format.
    //
    CxPlatConvertToMappedV6(RemoteAddress, &MappedRemoteAddress);

    BYTE CtrlBuf[
        WSA_CMSG_SPACE(sizeof(IN6_PKTINFO)) +   // IP_PKTINFO
        WSA_CMSG_SPACE(sizeof(INT)) +           // IP_ECN
#ifdef UDP_SEND_MSG_SIZE
        WSA_CMSG_SPACE(sizeof(DWORD))           // UDP_SEND_MSG_SIZE
#endif
        ];

    WSAMSG WSAMhdr;
    WSAMhdr.dwFlags = 0;
    if (Socket->HasFixedRemoteAddress) {
        WSAMhdr.name = NULL;
        WSAMhdr.namelen = 0;
    } else {
        WSAMhdr.name = (LPSOCKADDR)&MappedRemoteAddress;
        WSAMhdr.namelen = sizeof(MappedRemoteAddress);
    }
    WSAMhdr.lpBuffers = SendData->Buffers;
    WSAMhdr.dwBufferCount = SendData->BufferCount;
    WSAMhdr.Control.buf = (PCHAR)CtrlBuf;
    WSAMhdr.Control.len = 0;

    PWSACMSGHDR CMsg = NULL;
    if (LocalAddress->si_family == QUIC_ADDRESS_FAMILY_INET) {

        if (!Socket->HasFixedRemoteAddress) {
            WSAMhdr.Control.len += WSA_CMSG_SPACE(sizeof(IN_PKTINFO));
            CMsg = WSA_CMSG_FIRSTHDR(&WSAMhdr);
            CMsg->cmsg_level = IPPROTO_IP;
            CMsg->cmsg_type = IP_PKTINFO;
            CMsg->cmsg_len = WSA_CMSG_LEN(sizeof(IN_PKTINFO));
            PIN_PKTINFO PktInfo = (PIN_PKTINFO)WSA_CMSG_DATA(CMsg);
            PktInfo->ipi_ifindex = LocalAddress->Ipv6.sin6_scope_id;
            PktInfo->ipi_addr = LocalAddress->Ipv4.sin_addr;
        }

        WSAMhdr.Control.len += WSA_CMSG_SPACE(sizeof(INT));
        CMsg = WSA_CMSG_NXTHDR(&WSAMhdr, CMsg);
        CXPLAT_DBG_ASSERT(CMsg != NULL);
        CMsg->cmsg_level = IPPROTO_IP;
        CMsg->cmsg_type = IP_ECN;
        CMsg->cmsg_len = WSA_CMSG_LEN(sizeof(INT));
        *(PINT)WSA_CMSG_DATA(CMsg) = SendData->ECN;

    } else {

        if (!Socket->HasFixedRemoteAddress) {
            WSAMhdr.Control.len += WSA_CMSG_SPACE(sizeof(IN6_PKTINFO));
            CMsg = WSA_CMSG_FIRSTHDR(&WSAMhdr);
            CMsg->cmsg_level = IPPROTO_IPV6;
            CMsg->cmsg_type = IPV6_PKTINFO;
            CMsg->cmsg_len = WSA_CMSG_LEN(sizeof(IN6_PKTINFO));
            PIN6_PKTINFO PktInfo6 = (PIN6_PKTINFO)WSA_CMSG_DATA(CMsg);
            PktInfo6->ipi6_ifindex = LocalAddress->Ipv6.sin6_scope_id;
            PktInfo6->ipi6_addr = LocalAddress->Ipv6.sin6_addr;
        }

        WSAMhdr.Control.len += WSA_CMSG_SPACE(sizeof(INT));
        CMsg = WSA_CMSG_NXTHDR(&WSAMhdr, CMsg);
        CXPLAT_DBG_ASSERT(CMsg != NULL);
        CMsg->cmsg_level = IPPROTO_IPV6;
        CMsg->cmsg_type = IPV6_ECN;
        CMsg->cmsg_len = WSA_CMSG_LEN(sizeof(INT));
        *(PINT)WSA_CMSG_DATA(CMsg) = SendData->ECN;
    }

#ifdef UDP_SEND_MSG_SIZE
    if (SendData->SegmentSize > 0) {
        WSAMhdr.Control.len += WSA_CMSG_SPACE(sizeof(DWORD));
        CMsg = WSA_CMSG_NXTHDR(&WSAMhdr, CMsg);
        CXPLAT_DBG_ASSERT(CMsg != NULL);
        CMsg->cmsg_level = IPPROTO_UDP;
        CMsg->cmsg_type = UDP_SEND_MSG_SIZE;
        CMsg->cmsg_len = WSA_CMSG_LEN(sizeof(DWORD));
        *(PDWORD)WSA_CMSG_DATA(CMsg) = SendData->SegmentSize;
    }
#endif

    DWORD BytesSent = 0;

    int sendMsgRes =
        Socket->Datapath->WSASendMsg(
            SocketContext->SocketFd,
            &WSAMhdr,
            0,
            &BytesSent,
            NULL,
            NULL);

    if (sendMsgRes == SOCKET_ERROR) {
        int WSAError = WSAGetLastError();
        if (WSAError == WSAEWOULDBLOCK) {
            if (!IsPendedSend) {
                CxPlatLockAcquire(&SocketContext->PendingSendDataLock);
                CxPlatSocketContextPendSend(
                    SocketContext,
                    SendData,
                    LocalAddress,
                    RemoteAddress);
                CxPlatLockRelease(&SocketContext->PendingSendDataLock);
            }
            SendPending = TRUE;

            SOCK_NOTIFY_REGISTRATION reg = {
                .socket = SocketContext->SocketFd,
                .completionKey = &SocketContext->EventContexts[QUIC_SOCK_EVENT_SOCKET],
                .eventFilter = SOCK_NOTIFY_REGISTER_EVENTS_ALL,
                .operation = SOCK_NOTIFY_OP_ENABLE,
                .triggerFlags = SOCK_NOTIFY_TRIGGER_LEVEL | SOCK_NOTIFY_TRIGGER_PERSISTENT,
                .registrationResult = 0
            };

            DWORD Ret = ProcessSocketNotifications(
                SocketContext->ProcContext->CompletionPort,
                1,
                &reg,
                0,
                0,
                NULL,
                NULL
            );

            if (Ret != ERROR_SUCCESS) {
                QuicTraceEvent(
                    DatapathErrorStatus,
                    "[data][%p] ERROR, %u, %s.",
                    SocketContext->Binding,
                    Ret,
                    "ProcessSocketNotifications failed");
                Status = HRESULT_FROM_WIN32(Ret);
                goto Exit;
            }
            Status = QUIC_STATUS_PENDING;
            goto Exit;
        } else {
            Status = HRESULT_FROM_WIN32(WSAError);
            QuicTraceEvent(
                DatapathErrorStatus,
                "[data][%p] ERROR, %u, %s.",
                SocketContext->Binding,
                Status,
                "WSASendMsg failed");

            //
            // Unreachable events can sometimes come synchronously.
            // Send unreachable notification to MsQuic if any related
            // errors were received.
            //
            if (WSAError == WSAECONNREFUSED ||
                WSAError == WSAEHOSTUNREACH ||
                WSAError == WSAENETUNREACH) {
                if (!SocketContext->Binding->PcpBinding) {
                    SocketContext->Binding->Datapath->UdpHandlers.Unreachable(
                        SocketContext->Binding,
                        SocketContext->Binding->ClientContext,
                        &SocketContext->Binding->RemoteAddress);
                }
            }
            goto Exit;
        }
    }

    Status = QUIC_STATUS_SUCCESS;

Exit:

    if (!SendPending && !IsPendedSend) {
        // TODO Add TCP when necessary
        CxPlatSendDataComplete(SocketContext, SendData, Status);
    }

    return Status;
}

QUIC_STATUS
CxPlatSocketSend(
    _In_ CXPLAT_SOCKET* Socket,
    _In_ const CXPLAT_ROUTE* Route,
    _In_ CXPLAT_SEND_DATA* SendData,
    _In_ uint16_t IdealProcessor
    )
{
    UNREFERENCED_PARAMETER(IdealProcessor);
    QUIC_STATUS Status =
        CxPlatSocketSendInternal(
            Socket,
            &Route->LocalAddress,
            &Route->RemoteAddress,
            SendData,
            FALSE);
    if (Status == QUIC_STATUS_PENDING) {
        Status = QUIC_STATUS_SUCCESS;
    }
    return Status;
}

uint16_t
CxPlatSocketGetLocalMtu(
    _In_ CXPLAT_SOCKET* Socket
    )
{
    CXPLAT_DBG_ASSERT(Socket != NULL);
    return Socket->Mtu;
}

void
CxPlatDataPathWake(
    _In_ void* Context
    )
{
    CXPLAT_DATAPATH_PROC_CONTEXT* ProcContext = (CXPLAT_DATAPATH_PROC_CONTEXT*)Context;
    PostQueuedCompletionStatus(ProcContext->CompletionPort, 0, (ULONG_PTR)NULL, NULL);
}

#define OVERLAPPED_NUM_EVENTS 16 // TODO: Experiment.

void
CxPlatDataPathRunEC(
    _In_ void** Context,
    _In_ CXPLAT_THREAD_ID CurThreadId,
    _In_ uint32_t WaitTime
    )
{
    CXPLAT_DATAPATH_PROC_CONTEXT** EcProcContext = (CXPLAT_DATAPATH_PROC_CONTEXT**)Context;
    CXPLAT_DATAPATH_PROC_CONTEXT* ProcContext = *EcProcContext;
    CXPLAT_DBG_ASSERT(ProcContext->Datapath != NULL);

    OVERLAPPED_ENTRY OverlappedEvents[OVERLAPPED_NUM_EVENTS];

    ProcContext->ThreadId = CurThreadId;

    DWORD ReadyEventCount = 0;
    GetQueuedCompletionStatusEx(
        ProcContext->CompletionPort,
        OverlappedEvents,
        OVERLAPPED_NUM_EVENTS,
        &ReadyEventCount,
        WaitTime == UINT32_MAX ? INFINITE : (DWORD)WaitTime,
        FALSE);

    if (ProcContext->Datapath->Shutdown) {
        *Context = NULL;
        CxPlatEventSet(ProcContext->CompletionEvent);
        return;
    }

    if (ReadyEventCount == 0) {
        return; // Wake for timeout.
    }

    for (int i = 0; i < ReadyEventCount; i++) {
        if (OverlappedEvents[i].lpCompletionKey != NULL) {
            CxPlatSocketContextProcessEvents(
                OverlappedEvents[i].lpCompletionKey,
                SocketNotificationRetrieveEvents(&OverlappedEvents[i]));
        }
    }
}