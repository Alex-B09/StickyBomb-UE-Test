// Copyright Epic Games, Inc. All Rights Reserved.

#include "ShooterGame.h"
#include "Weapons/ShooterProjectile.h"
#include "Particles/ParticleSystemComponent.h"
#include "Effects/ShooterExplosionEffect.h"

AShooterProjectile::AShooterProjectile(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	CollisionComp = ObjectInitializer.CreateDefaultSubobject<USphereComponent>(this, TEXT("SphereComp"));
	CollisionComp->InitSphereRadius(5.0f);
	CollisionComp->AlwaysLoadOnClient = true;
	CollisionComp->AlwaysLoadOnServer = true;
	CollisionComp->bTraceComplexOnMove = true;
	CollisionComp->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	CollisionComp->SetCollisionObjectType(COLLISION_PROJECTILE);
	CollisionComp->SetCollisionResponseToAllChannels(ECR_Ignore);
	CollisionComp->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	CollisionComp->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	CollisionComp->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	RootComponent = CollisionComp;

    PickupSphereComp = ObjectInitializer.CreateDefaultSubobject<USphereComponent>(this, TEXT("PickupSphereComp"));
    PickupSphereComp->InitSphereRadius(25.f); // To define in BP
    PickupSphereComp->SetCollisionProfileName(COLLISION_PRESET_OPTIONALCOLLISION);
    PickupSphereComp->SetupAttachment(RootComponent);
    

	ParticleComp = ObjectInitializer.CreateDefaultSubobject<UParticleSystemComponent>(this, TEXT("ParticleComp"));
	ParticleComp->bAutoActivate = false;
	ParticleComp->bAutoDestroy = false;
	ParticleComp->SetupAttachment(RootComponent);

	MovementComp = ObjectInitializer.CreateDefaultSubobject<UProjectileMovementComponent>(this, TEXT("ProjectileComp"));
	MovementComp->UpdatedComponent = CollisionComp;
	MovementComp->InitialSpeed = 2000.0f;
	MovementComp->MaxSpeed = 2000.0f;
	MovementComp->bRotationFollowsVelocity = true;
	MovementComp->ProjectileGravityScale = 0.f;

	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
	SetRemoteRoleForBackwardsCompat(ROLE_SimulatedProxy);
	bReplicates = true;
	SetReplicatingMovement(true);
}

void AShooterProjectile::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	MovementComp->OnProjectileStop.AddDynamic(this, &AShooterProjectile::OnImpact);
	CollisionComp->MoveIgnoreActors.Add(GetInstigator());

	AShooterWeapon_Projectile* OwnerWeapon = Cast<AShooterWeapon_Projectile>(GetOwner());
	if (OwnerWeapon)
	{
		OwnerWeapon->ApplyWeaponConfig(WeaponConfig);
	}

	SetLifeSpan( WeaponConfig.ProjectileLife );
	MyController = GetInstigatorController();
}

void AShooterProjectile::BeginPlay()
{
    Super::BeginPlay();
    if (HasAuthority())
    {
        // set timer
        GetWorldTimerManager().SetTimer(ExplosionTimerHandle, this, &AShooterProjectile::Explode, 3.0f, false);

        // set collision
        PickupSphereComp->OnComponentBeginOverlap.AddDynamic(this, &AShooterProjectile::OnPickupOverlap);
        //PickupSphereComp->OnComponentEndOverlap.AddDynamic(this, &AShooterProjectile::OnPickupOverlapEnd);
    }
}

void AShooterProjectile::InitVelocity(FVector& ShootDirection)
{
	if (MovementComp)
	{
		MovementComp->Velocity = ShootDirection * MovementComp->InitialSpeed;
	}
}

void AShooterProjectile::OnImpact(const FHitResult& HitResult)
{
	if (GetLocalRole() == ROLE_Authority && !bExploded)
	{
        mLastHit = HitResult;
	}
}

void AShooterProjectile::Explode()
{
    UE_LOG(LogTemp, Log, TEXT("EXPLOOOOOOOOOOOOSION"));
	if (ParticleComp)
	{
		ParticleComp->Deactivate();
	}

    // get current position
    auto location = GetActorLocation();
    auto normal = GetActorForwardVector();

	// effects and damage origin shouldn't be placed inside mesh at impact point
	const FVector NudgedImpactLocation = location + normal * 10.0f;

	if (WeaponConfig.ExplosionDamage > 0 && WeaponConfig.ExplosionRadius > 0 && WeaponConfig.DamageType)
	{
		UGameplayStatics::ApplyRadialDamage(this, WeaponConfig.ExplosionDamage, NudgedImpactLocation, WeaponConfig.ExplosionRadius, WeaponConfig.DamageType, TArray<AActor*>(), this, MyController.Get());
	}

	if (ExplosionTemplate)
	{
		FTransform const SpawnTransform(GetActorRotation(), NudgedImpactLocation);
		AShooterExplosionEffect* const EffectActor = GetWorld()->SpawnActorDeferred<AShooterExplosionEffect>(ExplosionTemplate, SpawnTransform);
		if (EffectActor)
		{
            // use decal if the velocity is 0ish
            EffectActor->ShouldUseDecal = !(GetVelocity().IsNearlyZero());
            EffectActor->SurfaceHit = mLastHit;
			UGameplayStatics::FinishSpawningActor(EffectActor, SpawnTransform);
		}
	}

	bExploded = true; // this will trigger replication on the clients...i hope
                      // To test if this is problematic 
    DisableAndDestroy(); // this can only be called from the server
}

void AShooterProjectile::DisableAndDestroy()
{
	UAudioComponent* ProjAudioComp = FindComponentByClass<UAudioComponent>();
	if (ProjAudioComp && ProjAudioComp->IsPlaying())
	{
		ProjAudioComp->FadeOut(0.1f, 0.f);
	}

	MovementComp->StopMovementImmediately();

	// give clients some time to show explosion
	SetLifeSpan( 2.0f );
}

///CODE_SNIPPET_START: AActor::GetActorLocation AActor::GetActorRotation
void AShooterProjectile::OnRep_Exploded()
{
	FVector ProjDirection = GetActorForwardVector();

	const FVector StartTrace = GetActorLocation() - ProjDirection * 200;
	const FVector EndTrace = GetActorLocation() + ProjDirection * 150;
	FHitResult Impact;
	
	if (!GetWorld()->LineTraceSingleByChannel(Impact, StartTrace, EndTrace, COLLISION_PROJECTILE, FCollisionQueryParams(SCENE_QUERY_STAT(ProjClient), true, GetInstigator())))
	{
		// failsafe
		Impact.ImpactPoint = GetActorLocation();
		Impact.ImpactNormal = -ProjDirection;
	}

	//Explode(Impact);
}
///CODE_SNIPPET_END

void AShooterProjectile::PostNetReceiveVelocity(const FVector& NewVelocity)
{
	if (MovementComp)
	{
		MovementComp->Velocity = NewVelocity;
	}
}

void AShooterProjectile::GetLifetimeReplicatedProps( TArray< FLifetimeProperty > & OutLifetimeProps ) const
{
	Super::GetLifetimeReplicatedProps( OutLifetimeProps );
	
	DOREPLIFETIME( AShooterProjectile, bExploded );
}

void AShooterProjectile::OnPickupOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
    UE_LOG(LogTemp, Log, TEXT("Begin Overlap"));
}

void AShooterProjectile::OnPickupOverlapEnd(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
    UE_LOG(LogTemp, Log, TEXT("Begin End"));
}
