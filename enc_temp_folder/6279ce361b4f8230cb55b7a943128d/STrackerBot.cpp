// Fill out your copyright notice in the Description page of Project Settings.

#include "STrackerBot.h"
#include "Components/StaticMeshComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Runtime/NavigationSystem/Public/NavigationSystem.h"
#include "Runtime/NavigationSystem/Public/NavigationPath.h"
#include "GameFramework/Character.h"
#include "SHealthComponent.h"
#include "DrawDebugHelpers.h"
#include "SCharacter.h"
#include "Components/SphereComponent.h"
#include "Sound/SoundCue.h"

// Sets default values
ASTrackerBot::ASTrackerBot()
{
	// Set this pawn to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

	MeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MeshComp"));
	MeshComp->SetCanEverAffectNavigation(false);
	MeshComp->SetSimulatePhysics(true);
	RootComponent = MeshComp;

	HealthComp = CreateDefaultSubobject<USHealthComponent>(TEXT("HealthComp"));
	HealthComp->OnHealthChanged.AddDynamic(this, &ASTrackerBot::HandleDamage);

	SphereComp = CreateDefaultSubobject<USphereComponent>(TEXT("SphereComp"));
	SphereComp->SetSphereRadius(200);
	SphereComp->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	SphereComp->SetCollisionResponseToAllChannels(ECR_Ignore);
	SphereComp->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	SphereComp->SetupAttachment(RootComponent);

	bUseVelocityChange = false;
	MovementForce = 1000;
	RequiredDistanceToTarget = 100;

	ExplosionDamage = 100;
	ExplosionRadius = 40;
	SelfDamageInterval = 0.25f;
}

// Called when the game starts or when spawned
void ASTrackerBot::BeginPlay()
{
	Super::BeginPlay();

	if (Role == ROLE_Authority)
	{
		NextPathPoint = GetNextPathPoint();

		FTimerHandle TimerHandle_NearbyBots;
		GetWorldTimerManager().SetTimer(TimerHandle_NearbyBots, this, &ASTrackerBot::OnCheckNearbyBots, 1.0f, true, 0.0f);
	}
}

void ASTrackerBot::HandleDamage(USHealthComponent * OwningHealthComp, float Health, float HealthDelta, const UDamageType * DamageType, AController * InstigatedBy, AActor * DamageCauser)
{
	if (MatInst == nullptr)
	{
		MatInst = MeshComp->CreateAndSetMaterialInstanceDynamicFromMaterial(0, MeshComp->GetMaterial(0));
	}

	if (MatInst)
	{
		MatInst->SetScalarParameterValue("LastTimeDamageTaken", GetWorld()->TimeSeconds);
	}

	UE_LOG(LogTemp, Log, TEXT("Health of %s"), *FString::SanitizeFloat(Health), *GetName());

	if (Health <= 0)
	{
		SelfDestruct();
	}
}

FVector ASTrackerBot::GetNextPathPoint()
{
	TArray<AActor*> FoundActors;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), ACharacter::StaticClass(), FoundActors);

	if (FoundActors.Num() <= 0)
	{
		return GetActorLocation();
	}

	//Get Closest Player
	AActor* closestActor = nullptr;
	float currentClosestDistance = TNumericLimits<float>::Max();

	for (int i = 0; i < FoundActors.Num(); i++)
	{
		float distance = FVector::DistSquared(GetActorLocation(), FoundActors[i]->GetActorLocation());
		if (distance < currentClosestDistance)
		{
			currentClosestDistance = distance;
			closestActor = FoundActors[i];
		}
	}

	ACharacter* PlayerPawn = Cast<ASCharacter>(closestActor);
	UNavigationPath* Path = UNavigationSystemV1::FindPathToActorSynchronously(this, GetActorLocation(), PlayerPawn);

	//make sure we have more than one entry (first is actor location)
	if (Path && Path->PathPoints.Num() > 1)
	{
		//return next point in path
		return Path->PathPoints[1];
	}
	return GetActorLocation();
}

void ASTrackerBot::SelfDestruct()
{
	if (bExploded)
	{
		return;
	}

	bExploded = true;

	UGameplayStatics::SpawnEmitterAtLocation(GetWorld(), ExplosionEffect, GetActorLocation());
	UGameplayStatics::PlaySoundAtLocation(this, ExplodeSound, GetActorLocation());

	MeshComp->SetVisibility(false, true);
	MeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	if (Role == ROLE_Authority)
	{
		TArray<AActor*> IgnoredActors;
		IgnoredActors.Add(this);

		float ActualDamage = ExplosionDamage + (ExplosionDamage * PowerLevel);

		//Apply Damage
		UGameplayStatics::ApplyRadialDamage(this, ActualDamage, GetActorLocation(), ExplosionRadius, nullptr, IgnoredActors, this, GetInstigatorController(), true);

		DrawDebugSphere(GetWorld(), GetActorLocation(), ExplosionRadius, 12, FColor::Red, false, 2.0f, 0, 1.0f);

		//Destroy
		SetLifeSpan(2.0f);
	}
}

void ASTrackerBot::DamageSelf()
{
	UGameplayStatics::ApplyDamage(this, 20, GetInstigatorController(), this, nullptr);
}

// Called every frame
void ASTrackerBot::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	//Only on server
	if (Role == ROLE_Authority && !bExploded)
	{
		float DistanceToTarget = (GetActorLocation() - NextPathPoint).Size(); //distance between two points

		//IF we are close enough
		if (DistanceToTarget <= RequiredDistanceToTarget)
		{
			NextPathPoint = GetNextPathPoint();
		}
		else
		{
			//Keep moving towards next target
			FVector ForceDirection = NextPathPoint - GetActorLocation();
			ForceDirection.Normalize();
			ForceDirection *= MovementForce;
			MeshComp->AddForce(ForceDirection, NAME_None, bUseVelocityChange);
		}
	}

}

void ASTrackerBot::OnCheckNearbyBots()
{
	const float Radius = 600;

	FCollisionShape CollShape;
	CollShape.SetSphere(Radius);

	FCollisionObjectQueryParams QueryParams;
	QueryParams.AddObjectTypesToQuery(ECC_PhysicsBody);
	QueryParams.AddObjectTypesToQuery(ECC_Pawn);

	TArray<FOverlapResult> Overlaps;
	GetWorld()->OverlapMultiByObjectType(Overlaps, GetActorLocation(), FQuat::Identity, QueryParams, CollShape);

	DrawDebugSphere(GetWorld(), GetActorLocation(), Radius, 12, FColor::Cyan, 1.0f);

	//check overlapping bots
	int32 NumberOfBots = 0;
	for (FOverlapResult Result : Overlaps)
	{
		ASTrackerBot* Bot = Cast<ASTrackerBot>(Result.GetActor());

		if (Bot && Bot != this)
		{
			NumberOfBots++;
		}
	}

	const int32 MaxPowerLevel = 4;

	PowerLevel = FMath::Clamp(NumberOfBots, 0, MaxPowerLevel);

	if (MatInst == nullptr)
	{
		MatInst = MeshComp->CreateAndSetMaterialInstanceDynamicFromMaterial(0, MeshComp->GetMaterial(0));
	}
	if (MatInst)
	{
		float Alpha = PowerLevel / (float)MaxPowerLevel;

		MatInst->SetScalarParameterValue("PowerLevelAlpha", Alpha);
	}
}

void ASTrackerBot::NotifyActorBeginOverlap(AActor* OtherActor)
{
	if (!bStartedSelfDestruction && !bExploded)
	{
		ASCharacter* PlayerPawn = Cast<ASCharacter>(OtherActor);
		//we overlapped with player
		if (PlayerPawn)
		{
			if (Role == ROLE_Authority)
			{
				//start self destruct sequence
				GetWorldTimerManager().SetTimer(TimerHandle_SelfDamage, this, &ASTrackerBot::DamageSelf, SelfDamageInterval, true, 0.0f);
			}
			bStartedSelfDestruction = true;

			UGameplayStatics::SpawnSoundAttached(SelfDestructSound, RootComponent);
		}
	}
}

