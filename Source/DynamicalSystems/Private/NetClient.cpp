#include "NetClient.h"
#include "NetAvatar.h"
#include "NetVoice.h"
#include "NetRigidBody.h"
#include "RustyDynamics.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "DynamicalSystemsPrivatePCH.h"

DEFINE_LOG_CATEGORY(RustyNet);

ANetClient::ANetClient()
{
	PrimaryActorTick.bCanEverTick = true;
}

void ANetClient::RegisterRigidBody(UNetRigidBody* RigidBody)
{
	UE_LOG(RustyNet, Warning, TEXT("ANetClient::RegisterRigidBody %s"), *RigidBody->GetOwner()->GetName());
	NetRigidBodies.Add(RigidBody);
}

void ANetClient::RegisterAvatar(UNetAvatar* _Avatar)
{
    UE_LOG(RustyNet, Warning, TEXT("ANetClient::RegisterAvatar %i %s"), _Avatar->NetID, *_Avatar->GetOwner()->GetName());
    NetAvatars.Add(_Avatar);
}

void ANetClient::RegisterVoice(UNetVoice* Voice)
{
    UE_LOG(RustyNet, Warning, TEXT("ANetClient::RegisterVoice %s"), *Voice->GetOwner()->GetName());
    NetVoices.Add(Voice);
}

void ANetClient::Say(uint8* Bytes, uint32 Count)
{
}

void ANetClient::RebuildConsensus()
{
    int Count = NetClients.Num();
    FRandomStream Rnd(Count);
    
    MappedClients.Empty();
    NetClients.GetKeys(MappedClients);
    MappedClients.Sort();

	NetIndex = MappedClients.IndexOfByKey(Uuid);
    
    NetRigidBodies.Sort([](const UNetRigidBody& LHS, const UNetRigidBody& RHS) {
        return LHS.NetID > RHS.NetID; });
    for (auto It = NetRigidBodies.CreateConstIterator(); It; ++It) {
        (*It)->NetOwner = Rnd.RandRange(0, Count);
    }
}

void ANetClient::BeginPlay()
{
    Super::BeginPlay();
	Uuid = FMath::RandRange(100000, 999999);
    bool bCanBindAll;
    TSharedPtr<class FInternetAddr> localIp = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLocalHostAddr(*GLog, bCanBindAll);
    Local = localIp->ToString(true);
    UE_LOG(RustyNet, Warning, TEXT("GetLocalHostAddr %s"), *Local);
    LastPingTime = UGameplayStatics::GetRealTimeSeconds(GetWorld());
    LastAvatarTime = LastPingTime;
	LastRigidbodyTime = LastPingTime;
    if (Client == NULL) {
        Client = rd_netclient_open(TCHAR_TO_ANSI(*Local), TCHAR_TO_ANSI(*Server), TCHAR_TO_ANSI(*MumbleServer));
        NetClients.Add(Uuid, -1);
        UE_LOG(RustyNet, Warning, TEXT("NetClient BeginPlay %i"), Uuid);
    }
}

void ANetClient::BeginDestroy()
{
    Super::BeginDestroy();
    if (Client != NULL) {
		UE_LOG(RustyNet, Warning, TEXT("NetClient BeginDestroy %i"), Uuid);
        rd_netclient_drop(Client);
        Client = NULL;
    }
}

void ANetClient::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
    
    float CurrentTime = UGameplayStatics::GetRealTimeSeconds(GetWorld());
    float CurrentAvatarTime = CurrentTime;
	float CurrentRigidbodyTime = CurrentTime;
    
	for (int Idx=0; Idx<NetRigidBodies.Num();) {
		if (!IsValid(NetRigidBodies[Idx])) {
			NetRigidBodies.RemoveAt(Idx);
		}
		else {
			++Idx;
		}
	}

    for (int Idx=0; Idx<NetAvatars.Num();) {
        if (!IsValid(NetAvatars[Idx])) {
            NetAvatars.RemoveAt(Idx);
        }
        else {
            ++Idx;
        }
    }
    
    for (int Idx=0; Idx<NetVoices.Num();) {
        if (!IsValid(NetVoices[Idx])) {
            NetVoices.RemoveAt(Idx);
        }
        else {
            ++Idx;
        }
    }
    
	{
        TArray<int32> DeleteList;
        for (auto& Elem : NetClients) {
            if (Elem.Value > 0 && (CurrentTime - Elem.Value) > 20) {
				UE_LOG(RustyNet, Warning, TEXT("NetClient DELETED: %i"), Elem.Key);
                DeleteList.Add(Elem.Key);
            }
        }
        for (auto& Key : DeleteList) {
            NetClients.Remove(Key);
			RebuildConsensus();
        }
    }
    
    if (CurrentTime > LastPingTime + 1) { // Ping
		uint8 Msg[5];
        Msg[0] = 0;
		//TODO: byte order
		uint8* bytes = (uint8*)(&Uuid);
		Msg[1] = bytes[0];
		Msg[2] = bytes[1];
		Msg[3] = bytes[2];
		Msg[4] = bytes[3];
        rd_netclient_msg_push(Client, Msg, 5);
        LastPingTime = CurrentTime;
    }
    
    if (CurrentAvatarTime > LastAvatarTime + 0.1) { // Avatar
		if (IsValid(Avatar) && !Avatar->IsNetProxy) {
			AvatarPack Pack;
			memset(&Pack, 0, sizeof(AvatarPack));
			FQuat Rotation;
			Pack.id = Avatar->NetID;
			Pack.root_px = Avatar->Location.X; Pack.root_py = Avatar->Location.Y; Pack.root_pz = Avatar->Location.Z;
			Rotation = Avatar->Rotation.Quaternion();
			Pack.root_rx = Rotation.X; Pack.root_ry = Rotation.Y; Pack.root_rz = Rotation.Z; Pack.root_rw = Rotation.W;
			Pack.head_px = Avatar->LocationHMD.X; Pack.head_py = Avatar->LocationHMD.Y; Pack.head_pz = Avatar->LocationHMD.Z;
			Rotation = Avatar->RotationHMD.Quaternion();
			Pack.head_rx = Rotation.X; Pack.head_ry = Rotation.Y; Pack.head_rz = Rotation.Z; Pack.head_rw = Rotation.W;
			Pack.handL_px = Avatar->LocationHandL.X; Pack.handL_py = Avatar->LocationHandL.Y; Pack.handL_pz = Avatar->LocationHandL.Z;
			Rotation = Avatar->RotationHandL.Quaternion();
			Pack.handL_rx = Rotation.X; Pack.handL_ry = Rotation.Y; Pack.handL_rz = Rotation.Z; Pack.handL_rw = Rotation.W;
			Pack.handR_px = Avatar->LocationHandR.X; Pack.handR_py = Avatar->LocationHandR.Y; Pack.handR_pz = Avatar->LocationHandR.Z;
			Rotation = Avatar->RotationHandR.Quaternion();
			Pack.handR_rx = Rotation.X; Pack.handR_ry = Rotation.Y; Pack.handR_rz = Rotation.Z; Pack.handR_rw = Rotation.W;
			Pack.height = Avatar->Height;
			Pack.floor = Avatar->Floor;
			Pack.GripL = Avatar->GripL;
			Pack.GripR = Avatar->GripR;
			
			rd_netclient_push_avatar(Client, &Pack);
		}
        LastAvatarTime = CurrentAvatarTime;
    }

	if (CurrentRigidbodyTime > LastRigidbodyTime + 0.1) { // Rigidbody
		for (int Idx=0; Idx<NetRigidBodies.Num(); ++Idx) {
		    UNetRigidBody* Body = NetRigidBodies[Idx];
		    if (IsValid(Body) && Body->NetOwner == NetIndex) {
		        AActor* Actor = Body->GetOwner();
		        if (IsValid(Actor)) {
		            FVector Location = Actor->GetActorLocation();
					FQuat Rotation = Actor->GetActorRotation().Quaternion();
		            UStaticMeshComponent* StaticMesh = Actor->FindComponentByClass<UStaticMeshComponent>();
		            if (StaticMesh) {
						FVector LinearVelocity = StaticMesh->GetBodyInstance()->GetUnrealWorldVelocity();
						FVector AngularVelocity = StaticMesh->GetBodyInstance()->GetUnrealWorldAngularVelocity();
		                RigidbodyPack Pack = {Body->NetID,
		                    Location.X, Location.Y, Location.Z, 1,
		                    LinearVelocity.X, LinearVelocity.Y, LinearVelocity.Z, 0,
							Rotation.X, Rotation.Y, Rotation.Z, Rotation.W,
							AngularVelocity.X, AngularVelocity.Y, AngularVelocity.Z, 0
		                };
						rd_netclient_push_rigidbody(Client, &Pack);
		            }
		        }
		    }
		}
		LastRigidbodyTime = CurrentRigidbodyTime;
	}
    
	int Loop = 0;
	for (; Loop < 1000; Loop += 1) {
		RustVec* RustMsg = rd_netclient_msg_pop(Client);
		uint8* Msg = (uint8*)RustMsg->vec_ptr;
		if (RustMsg->vec_len > 0) {

			if (Msg[0] == 0) { // Ping
				uint32 RemoteUuid = *((uint32*)(Msg + 1));
				// float* KeyValue = NetClients.Find(RemoteUuid);
				// if (KeyValue != NULL) {
				UE_LOG(RustyNet, Warning, TEXT("PING: %i"), RemoteUuid);
				// }
				NetClients.Add(RemoteUuid, CurrentTime);
				RebuildConsensus();
			}
			else if (Msg[0] == 1) { // World
			}
			else if (Msg[0] == 2) { // Avatar
				AvatarPack* Pack = rd_netclient_dec_avatar(&Msg[1], RustMsg->vec_len - 1);
				uint32 NetID = Pack->id;
				UNetAvatar** NetAvatar = NetAvatars.FindByPredicate([NetID](const UNetAvatar* Item) {
					return IsValid(Item) && Item->NetID == NetID;
				});
				if (NetAvatar != NULL && *NetAvatar != NULL) {
					(*NetAvatar)->LastUpdateTime = CurrentTime;
					(*NetAvatar)->Location = FVector(Pack->root_px, Pack->root_py, Pack->root_pz);
					(*NetAvatar)->Rotation = FRotator(FQuat(Pack->root_rx, Pack->root_ry, Pack->root_rz, Pack->root_rw));
					(*NetAvatar)->LocationHMD = FVector(Pack->head_px, Pack->head_py, Pack->head_pz);
					(*NetAvatar)->RotationHMD = FRotator(FQuat(Pack->head_rx, Pack->head_ry, Pack->head_rz, Pack->head_rw));
					(*NetAvatar)->LocationHandL = FVector(Pack->handL_px, Pack->handL_py, Pack->handL_pz);
					(*NetAvatar)->RotationHandL = FRotator(FQuat(Pack->handL_rx, Pack->handL_ry, Pack->handL_rz, Pack->handL_rw));
					(*NetAvatar)->LocationHandR = FVector(Pack->handR_px, Pack->handR_py, Pack->handR_pz);
					(*NetAvatar)->RotationHandR = FRotator(FQuat(Pack->handR_rx, Pack->handR_ry, Pack->handR_rz, Pack->handR_rw));
					(*NetAvatar)->Height = Pack->height;
					(*NetAvatar)->Floor = Pack->floor;
					(*NetAvatar)->GripL = Pack->GripL;
					(*NetAvatar)->GripR = Pack->GripR;

				}
				else {
					MissingAvatar = (int)NetID;
					UE_LOG(RustyNet, Warning, TEXT("NetClient MissingAvatar: %i"), NetID);
				}
				rd_netclient_drop_avatar(Pack);
			}
			else if (Msg[0] == 3) { // Rigidbody
				RigidbodyPack* Pack = rd_netclient_dec_rigidbody(&Msg[1], RustMsg->vec_len - 1);
				uint32 NetID = Pack->id;
				UNetRigidBody** NetRigidBody = NetRigidBodies.FindByPredicate([NetID](const UNetRigidBody* Item) {
					return IsValid(Item) && Item->NetID == NetID;
				});
				if (NetRigidBody != NULL && *NetRigidBody != NULL) {
					(*NetRigidBody)->TargetLocation = FVector(Pack->px, Pack->py, Pack->pz);
					(*NetRigidBody)->TargetLinearVelocity = FVector(Pack->lx, Pack->ly, Pack->lz);
					(*NetRigidBody)->TargetRotation = FRotator(FQuat(Pack->rx, Pack->ry, Pack->rz, Pack->rw));
					(*NetRigidBody)->TargetAngularVelocity = FVector(Pack->ax, Pack->ay, Pack->az);
				}
				rd_netclient_drop_rigidbody(Pack);
			}
			else if (Msg[0] == 10) { // System Float
				uint8 MsgSystem = Msg[1];
				uint8 MsgId = Msg[2];
				float* MsgValue = (float*)(Msg + 3);
				UE_LOG(RustyNet, Warning, TEXT("Msg IN MsgSystem: %u MsgId: %u MsgValue: %f"), Msg[1], Msg[2], *MsgValue);
				OnSystemFloatMsg.Broadcast(MsgSystem, MsgId, *MsgValue);
			}
			else if (Msg[0] == 11) { // System Int
				uint8 MsgSystem = Msg[1];
				uint8 MsgId = Msg[2];
				int32* MsgValue = (int32*)(Msg + 3);
				UE_LOG(RustyNet, Warning, TEXT("Msg IN MsgSystem: %u MsgId: %u MsgValue: %i"), Msg[1], Msg[2], *MsgValue);
				OnSystemIntMsg.Broadcast(MsgSystem, MsgId, *MsgValue);
			}
			else if (Msg[0] == 12) { // System String
				uint8 MsgSystem = Msg[1];
				uint8 MsgId = Msg[2];
				const char* MsgValuePtr = (const char*)(Msg + 3);
				FString MsgValue(MsgValuePtr);
				UE_LOG(RustyNet, Warning, TEXT("Msg IN MsgSystem: %u MsgId: %u MsgValue: %s"), Msg[1], Msg[2], *MsgValue);
				OnSystemStringMsg.Broadcast(MsgSystem, MsgId, *MsgValue);
			}
		}
		rd_netclient_msg_drop(RustMsg);
		if (RustMsg->vec_len == 0) {
			//UE_LOG(RustyNet, Warning, TEXT("NetClient Loop: %i"), Loop);
			break;
		}
	}
}

void ANetClient::SendSystemFloat(int32 System, int32 Id, float Value)
{
	uint8 Msg[7];
	Msg[0] = 10;
	Msg[1] = (uint8)System;
	Msg[2] = (uint8)Id;

	//TODO: byte order
	uint8* fbytes = (uint8*)(&Value);
	Msg[3] = fbytes[0];
	Msg[4] = fbytes[1];
	Msg[5] = fbytes[2];
	Msg[6] = fbytes[3];

	UE_LOG(RustyNet, Warning, TEXT("Msg OUT System Float: %u MsgId: %u MsgValue: %f"), Msg[1], Msg[2], Value);
	rd_netclient_msg_push(Client, Msg, 7);
}

void ANetClient::SendSystemInt(int32 System, int32 Id, int32 Value)
{
	uint8 Msg[7];
	Msg[0] = 11;
	Msg[1] = (uint8)System;
	Msg[2] = (uint8)Id;

	//TODO: byte order
	uint8* ibytes = (uint8*)(&Value);
	Msg[3] = ibytes[0];
	Msg[4] = ibytes[1];
	Msg[5] = ibytes[2];
	Msg[6] = ibytes[3];

	UE_LOG(RustyNet, Warning, TEXT("Msg OUT System Int: %u MsgId: %u MsgValue: %i"), Msg[1], Msg[2], Value);
	rd_netclient_msg_push(Client, Msg, 7);
}

void ANetClient::SendSystemString(int32 System, int32 Id, FString Value)
{
	uint8 Msg[2000];
	memset(Msg, 0, 2000);

	Msg[0] = 12;
	Msg[1] = (uint8)System;
	Msg[2] = (uint8)Id;

	const char* String = TCHAR_TO_ANSI(*Value);
	uint32 StringLen = (uint32)strnlen(String, 1000);
	strncpy((char*)(Msg + 3), String, StringLen);

	UE_LOG(RustyNet, Warning, TEXT("Msg OUT System Int: %u MsgId: %u MsgValue: %s"), Msg[1], Msg[2], *Value);
	rd_netclient_msg_push(Client, Msg, StringLen + 10);
}

