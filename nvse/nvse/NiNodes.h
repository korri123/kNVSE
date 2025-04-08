#pragma once

#include <cassert>
#include <optional>
#include <ranges>
#include <span>

#include "GameForms.h"
#include "NiTypes.h"
#include "GameTypes.h"
#include "Utilities.h"
#include "nvse_plugin_example/containers.h"

constexpr float NI_INFINITY = FLT_MAX;
constexpr float INVALID_TIME = -NI_INFINITY;
constexpr unsigned char INVALID_INDEX = UCHAR_MAX;

#define GetBit(x) ((m_uFlags & x) != 0)
#define SetBit(x, y) (x ? (m_uFlags |= y) : (m_uFlags &= ~y))

inline float NiAbs (float fValue)
{
	return float(fabs(fValue));
}

template <typename T>
T* NiNew()
{
	return CdeclCall<T*>(0xAA13E0, sizeof(T));
}

template <typename T>
void NiDelete(T* ptr)
{
	CdeclCall<void>(0xAA1460, ptr, sizeof(T));
}

/*** class hierarchy
 *
 *	yet again taken from rtti information
 *	ni doesn't seem to use multiple inheritance
 *
 *	thanks to the NifTools team for their work on the on-disk format
 *	thanks to netimmerse for NiObject::DumpAttributes
 *
 *	all offsets here are assuming patch 1.2 as they changed dramatically
 *	0xE8 bytes were removed from NiObjectNET, and basically everything derives from that
 *
 *	NiObject derives from NiRefObject
 *
 *	BSFaceGenMorphData - derived from NiRefObject
 *		BSFaceGenMorphDataHead
 *		BSFaceGenMorphDataHair
 *
 *	BSTempEffect - derived from NiObject
 *		BSTempEffectDecal
 *		BSTempEffectGeometryDecal
 *		BSTempEffectParticle
 *		MagicHitEffect
 *			MagicModelHitEffect
 *			MagicShaderHitEffect
 *
 *	NiDX92DBufferData - derived from NiRefObject and something else
 *		NiDX9DepthStencilBufferData
 *			NiDX9SwapChainDepthStencilBufferData
 *			NiDX9ImplicitDepthStencilBufferData
 *			NiDX9AdditionalDepthStencilBufferData
 *		NiDX9TextureBufferData
 *		NiDX9OnscreenBufferData
 *			NiDX9SwapChainBufferData
 *			NiDX9ImplicitBufferData
 *
 *	NiObject
 *		NiObjectNET
 *			NiProperty
 *				NiTexturingProperty
 *				NiVertexColorProperty
 *				NiWireframeProperty
 *				NiZBufferProperty
 *				NiMaterialProperty
 *				NiAlphaProperty
 *				NiStencilProperty
 *				NiRendererSpecificProperty
 *				NiShadeProperty
 *					BSShaderProperty
 *						SkyShaderProperty
 *						ParticleShaderProperty
 *						BSShaderLightingProperty
 *							DistantLODShaderProperty
 *							TallGrassShaderProperty
 *							BSShaderPPLightingProperty
 *								SpeedTreeShaderPPLightingProperty
 *									SpeedTreeBranchShaderProperty
 *								Lighting30ShaderProperty
 *								HairShaderProperty
 *							SpeedTreeShaderLightingProperty
 *								SpeedTreeLeafShaderProperty
 *								SpeedTreeFrondShaderProperty
 *							GeometryDecalShaderProperty
 *						PrecipitationShaderProperty
 *						BoltShaderProperty
 *						WaterShaderProperty
 *				NiSpecularProperty
 *				NiFogProperty
 *					BSFogProperty
 *				NiDitherProperty
 *			NiTexture
 *				NiDX9Direct3DTexture
 *				NiSourceTexture
 *					NiSourceCubeMap
 *				NiRenderedTexture
 *					NiRenderedCubeMap
 *			NiAVObject
 *				NiDynamicEffect
 *					NiLight
 *						NiDirectionalLight
 *						NiPointLight
 *							NiSpotLight
 *						NiAmbientLight
 *					NiTextureEffect
 *				NiNode
 *					SceneGraph
 *					BSTempNodeManager
 *					BSTempNode
 *					BSCellNode
 *					BSClearZNode
 *					BSFadeNode
 *					BSScissorNode
 *					BSTimingNode
 *					BSFaceGenNiNode
 *					NiBillboardNode
 *					NiSwitchNode
 *						NiLODNode
 *							NiBSLODNode
 *					NiSortAdjustNode
 *					NiBSPNode
 *					ShadowSceneNode
 *				NiCamera
 *					BSCubeMapCamera - RTTI data incorrect
 *					NiScreenSpaceCamera
 *				NiGeometry
 *					NiLines
 *					NiTriBasedGeom
 *						NiTriShape
 *							BSScissorTriShape
 *							NiScreenElements
 *							NiScreenGeometry
 *							TallGrassTriShape
 *						NiTriStrips
 *							TallGrassTriStrips
 *					NiParticles
 *						NiParticleSystem
 *							NiMeshParticleSystem
 *						NiParticleMeshes
 *			NiSequenceStreamHelper
 *		NiRenderer
 *			NiDX9Renderer
 *		NiPixelData
 *		NiCollisionObject
 *			NiCollisionData
 *			bhkNiCollisionObject
 *				bhkCollisionObject
 *					bhkBlendCollisionObject
 *						WeaponObject
 *						bhkBlendCollisionObjectAddRotation
 *				bhkPCollisionObject
 *					bhkSPCollisionObject
 *		NiControllerSequence
 *			BSAnimGroupSequence
 *		NiTimeController
 *			BSDoorHavokController
 *			BSPlayerDistanceCheckController
 *			NiD3DController
 *			NiControllerManager
 *			NiInterpController
 *				NiSingleInterpController
 *					NiTransformController
 *					NiPSysModifierCtlr
 *						NiPSysEmitterCtlr
 *						NiPSysModifierBoolCtlr
 *							NiPSysModifierActiveCtlr
 *						NiPSysModifierFloatCtlr
 *							NiPSysInitialRotSpeedVarCtlr
 *							NiPSysInitialRotSpeedCtlr
 *							NiPSysInitialRotAngleVarCtlr
 *							NiPSysInitialRotAngleCtlr
 *							NiPSysGravityStrengthCtlr
 *							NiPSysFieldMaxDistanceCtlr
 *							NiPSysFieldMagnitudeCtlr
 *							NiPSysFieldAttenuationCtlr
 *							NiPSysEmitterSpeedCtlr
 *							NiPSysEmitterPlanarAngleVarCtlr
 *							NiPSysEmitterPlanarAngleCtlr
 *							NiPSysEmitterLifeSpanCtlr
 *							NiPSysEmitterInitialRadiusCtlr
 *							NiPSysEmitterDeclinationVarCtlr
 *							NiPSysEmitterDeclinationCtlr
 *							NiPSysAirFieldSpreadCtlr
 *							NiPSysAirFieldInheritVelocityCtlr
 *							NiPSysAirFieldAirFrictionCtlr
 *					NiFloatInterpController
 *						NiFlipController
 *						NiAlphaController
 *						NiTextureTransformController
 *						NiLightDimmerController
 *					NiBoolInterpController
 *						NiVisController
 *					NiPoint3InterpController
 *						NiMaterialColorController
 *						NiLightColorController
 *					NiExtraDataController
 *						NiFloatsExtraDataPoint3Controller
 *						NiFloatsExtraDataController
 *						NiFloatExtraDataController
 *						NiColorExtraDataController
 *				NiMultiTargetTransformController
 *				NiGeomMorpherController
 *			bhkBlendController
 *			bhkForceController
 *			NiBSBoneLODController
 *			NiUVController
 *			NiPathController
 *			NiLookAtController
 *			NiKeyframeManager
 *			NiBoneLODController
 *			NiPSysUpdateCtlr
 *			NiPSysResetOnLoopCtlr
 *			NiFloatController
 *				NiRollController
 *		bhkRefObject
 *			bhkSerializable
 *				bhkWorld - NiRTTI has incorrect parent
 *					bhkWorldM
 *				bhkAction
 *					bhkUnaryAction
 *						bhkMouseSpringAction
 *						bhkMotorAction
 *					bhkBinaryAction
 *						bhkSpringAction
 *						bhkAngularDashpotAction
 *						bhkDashpotAction
 *				bhkWorldObject
 *					bhkPhantom
 *						bhkShapePhantom
 *							bhkSimpleShapePhantom
 *							bhkCachingShapePhantom
 *						bhkAabbPhantom
 *							bhkAvoidBox
 *					bhkEntity
 *						bhkRigidBody
 *							bhkRigidBodyT
 *				bhkConstraint
 *					bhkLimitedHingeConstraint
 *					bhkMalleableConstraint
 *					bhkBreakableConstraint
 *					bhkWheelConstraint
 *					bhkStiffSpringConstraint
 *					bhkRagdollConstraint
 *					bhkPrismaticConstraint
 *					bhkHingeConstraint
 *					bhkBallAndSocketConstraint
 *					bhkGenericConstraint
 *						bhkFixedConstraint
 *					bhkPointToPathConstraint
 *					bhkPoweredHingeConstraint
 *				bhkShape
 *					bhkTransformShape
 *					bhkSphereRepShape
 *						bhkConvexShape
 *							bhkSphereShape
 *							bhkCapsuleShape
 *							bhkBoxShape
 *							bhkTriangleShape
 *							bhkCylinderShape
 *							bhkConvexVerticesShape
 *								bhkCharControllerShape
 *							bhkConvexTransformShape
 *							bhkConvexSweepShape
 *						bhkMultiSphereShape
 *					bhkBvTreeShape
 *						bhkTriSampledHeightFieldBvTreeShape
 *						bhkMoppBvTreeShape
 *					bhkShapeCollection
 *						bhkListShape
 *						bhkPackedNiTriStripsShape
 *						bhkNiTriStripsShape
 *					bhkHeightFieldShape
 *						bhkPlaneShape
 *				bhkCharacterProxy
 *					bhkCharacterListenerArrow - no NiRTTI
 *					bhkCharacterListenerSpell - no NiRTTI
 *					bhkCharacterController - no NiRTTI
 *		NiExtraData
 *			TESObjectExtraData
 *			BSFaceGenAnimationData
 *			BSFaceGenModelExtraData
 *			BSFaceGenBaseMorphExtraData
 *			DebugTextExtraData
 *			NiStringExtraData
 *			NiFloatExtraData
 *				FadeNodeMaxAlphaExtraData
 *			BSFurnitureMarker
 *			NiBinaryExtraData
 *			BSBound
 *			NiSCMExtraData
 *			NiTextKeyExtraData
 *			NiVertWeightsExtraData
 *			bhkExtraData
 *			PArrayPoint
 *			NiIntegerExtraData
 *				BSXFlags
 *			NiFloatsExtraData
 *			NiColorExtraData
 *			NiVectorExtraData
 *			NiSwitchStringExtraData
 *			NiStringsExtraData
 *			NiIntegersExtraData
 *			NiBooleanExtraData
 *		NiAdditionalGeometryData
 *			BSPackedAdditionalGeometryData
 *		NiGeometryData
 *			NiLinesData
 *			NiTriBasedGeomData
 *				NiTriStripsData
 *					NiTriStripsDynamicData
 *				NiTriShapeData
 *					NiScreenElementsData
 *					NiTriShapeDynamicData
 *					NiScreenGeometryData
 *			NiParticlesData
 *				NiPSysData
 *					NiMeshPSysData
 *				NiParticleMeshesData
 *		NiTask
 *			BSTECreateTask
 *			NiParallelUpdateTaskManager::SignalTask
 *			NiGeomMorpherUpdateTask
 *			NiPSysUpdateTask
 *		NiSkinInstance
 *		NiSkinPartition
 *		NiSkinData
 *		NiRenderTargetGroup
 *		Ni2DBuffer
 *			NiDepthStencilBuffer
 *		NiUVData
 *		NiStringPalette
 *		NiSequence
 *		NiRotData
 *		NiPosData
 *		NiMorphData
 *		NiTransformData
 *		NiFloatData
 *		NiColorData
 *		NiBSplineData
 *		NiBSplineBasisData
 *		NiBoolData
 *		NiTaskManager
 *			NiParallelUpdateTaskManager
 *		hkPackedNiTriStripsData
 *		NiInterpolator
 *			NiBlendInterpolator
 *				NiBlendTransformInterpolator
 *				NiBlendAccumTransformInterpolator
 *				NiBlendFloatInterpolator
 *				NiBlendQuaternionInterpolator
 *				NiBlendPoint3Interpolator
 *				NiBlendColorInterpolator
 *				NiBlendBoolInterpolator
 *			NiLookAtInterpolator
 *			NiKeyBasedInterpolator
 *				NiFloatInterpolator
 *				NiTransformInterpolator
 *				NiQuaternionInterpolator
 *				NiPoint3Interpolator
 *				NiPathInterpolator
 *				NiColorInterpolator
 *				NiBoolInterpolator
 *					NiBoolTimelineInterpolator
 *			NiBSplineInterpolator
 *				NiBSplineTransformInterpolator
 *					NiBSplineCompTransformInterpolator
 *				NiBSplinePoint3Interpolator
 *					NiBSplineCompPoint3Interpolator
 *				NiBSplineFloatInterpolator
 *					NiBSplineCompFloatInterpolator
 *				NiBSplineColorInterpolator
 *					NiBSplineCompColorInterpolator
 *		NiAVObjectPalette
 *			NiDefaultAVObjectPalette
 *		BSReference
 *		BSNodeReferences
 *		NiPalette
 *		NiLODData
 *			NiRangeLODData
 *			NiScreenLODData
 *		NiPSysModifier
 *			BSWindModifier
 *			NiPSysMeshUpdateModifier
 *			NiPSysRotationModifier
 *			NiPSysEmitter
 *				NiPSysMeshEmitter
 *				NiPSysVolumeEmitter
 *					NiPSysCylinderEmitter
 *					NiPSysSphereEmitter
 *					NiPSysBoxEmitter
 *					BSPSysArrayEmitter
 *			NiPSysGravityModifier
 *			NiPSysSpawnModifier
 *			BSParentVelocityModifier
 *			NiPSysPositionModifier
 *			NiPSysGrowFadeModifier
 *			NiPSysDragModifier
 *			NiPSysColorModifier
 *			NiPSysColliderManager
 *			NiPSysBoundUpdateModifier
 *			NiPSysBombModifier
 *			NiPSysAgeDeathModifier
 *			NiPSysFieldModifier
 *				NiPSysVortexFieldModifier
 *				NiPSysTurbulenceFieldModifier
 *				NiPSysRadialFieldModifier
 *				NiPSysGravityFieldModifier
 *				NiPSysDragFieldModifier
 *				NiPSysAirFieldModifier
 *		NiPSysEmitterCtlrData
 *		NiAccumulator
 *			NiBackToFrontAccumulator
 *				NiAlphaAccumulator
 *					BSShaderAccumulator
 *		NiScreenPolygon
 *		NiScreenTexture
 *		NiPSysCollider
 *			NiPSysSphericalCollider
 *			NiPSysPlanarCollider
 *
 *	NiShader
 *		NiD3DShaderInterface
 *			NiD3DShader
 *				NiD3DDefaultShader
 *					SkyShader
 *					ShadowLightShader
 *						ParallaxShader
 *						SkinShader
 *						HairShader
 *						SpeedTreeBranchShader
 *					WaterShaderHeightMap
 *					WaterShader
 *					WaterShaderDisplacement
 *					ParticleShader
 *					TallGrassShader
 *					PrecipitationShader
 *					SpeedTreeLeafShader
 *					BoltShader
 *					Lighting30Shader
 *					GeometryDecalShader
 *					SpeedTreeFrondShader
 *					DistantLODShader
 *
 *	NiD3DShaderConstantMap
 *		NiD3DSCM_Vertex
 *		NiD3DSCM_Pixel
 *
 ****/

struct NiFloatKey;
class NiControllerSequence;
class NiTransformInterpolator;
class NiAVObject;
class BSFadeNode;
class NiExtraData;
class NiTimeController;
class NiControllerManager;
class NiStringPalette;
class NiTextKeyExtraData;
class NiCamera;
class NiProperty;
class NiStream;
class TESAnimGroup;
class NiNode;
class NiGeometry;
class ParticleShaderProperty;
class TESObjectCELL;
class TESObjectREFR;
class TESEffectShader;
class ActiveEffect;

struct NiFixedString
{
	char* data;

	NiFixedString()
		: data(nullptr)
	{
	}

	NiFixedString(const NiFixedString& other);

	NiFixedString& operator=(const NiFixedString& other);

	NiFixedString(NiFixedString&& other) noexcept;

	~NiFixedString();

	const char* CStr() const
	{
		return data;
	}

	NiFixedString(const char* data);

	void Set(const char* newString);

	operator char*() const
	{
		return data;
	}

	operator const char*() const
	{
		return data;
	}

	operator std::string_view() const
	{
		return data ? data : "";
	}

	std::string_view Str() const
	{
		return data ? data : "";
	}

	// assignment operator
	NiFixedString& operator=(const char* newString)
	{
		Set(newString);
		return *this;
	}

	bool operator==(const NiFixedString& other) const
	{
		return data == other.data;
	}

};

// member fn addresses
#if RUNTIME
	const UInt32 kNiObjectNET_GetExtraData = 0x006FF9C0;
#endif

struct NiMemObject
{
};

// 008
class NiRefObject : public NiMemObject
{
public:
	NiRefObject();

	virtual ~NiRefObject();								// 00
	virtual void		DeleteThis();					// 01

//	void		** _vtbl;		// 000
	UInt32		m_uiRefCount;	// 004 - name known

	void IncrementRefCount()
	{
		InterlockedIncrement(&m_uiRefCount);
	}

	void DecrementRefCount()
	{
		if (InterlockedDecrement(&m_uiRefCount) == 0)
			DeleteThis();
	}
};

// 008
class NiObject : public NiRefObject
{
public:
	NiObject();
	~NiObject();

	virtual NiRTTI *	GetType(void);		// 02
	virtual NiNode *	GetAsNiNode(void);	// 03 
	virtual UInt32		Unk_04(void);		// 04
	virtual UInt32		Unk_05(void);		// 05
	virtual UInt32		Unk_06(void);		// 06
	virtual UInt32		Unk_07(void);		// 07
	virtual UInt32		Unk_08(void);		// 08
	virtual UInt32		Unk_09(void);		// 09
	virtual UInt32		Unk_0A(void);		// 0A
	virtual UInt32		Unk_0B(void);		// 0B
	virtual UInt32		Unk_0C(void);		// 0C
	virtual UInt32		Unk_0D(void);		// 0D
	virtual UInt32		Unk_0E(void);		// 0E
	virtual UInt32		Unk_0F(void);		// 0F
	virtual UInt32		Unk_10(void);		// 10
	virtual UInt32		Unk_11(void);		// 11
	virtual NiObject *	Copy(void);			// 12 (returns this, GetAsNiObject ?). Big doubt with everything below, except last which is 022
	virtual void		Load(NiStream * stream);
	virtual void		PostLoad(NiStream * stream);
	virtual void		FindNodes(NiStream * stream);	// give NiStream all of the NiNodes we own
	virtual void		Save(NiStream * stream);
	virtual bool		Compare(NiObject * obj);
	virtual void		DumpAttributes(NiTArray <char *> * dst);
	virtual void		DumpChildAttributes(NiTArray <char *> * dst);
	virtual void		Unk_1A(void);
	virtual void		Unk_1B(UInt32 arg);
	virtual void		Unk_1C(void);
	virtual void		GetType2(void);					// calls GetType
	virtual void		Unk_1E(UInt32 arg);
	virtual void		Unk_1F(void);
	virtual void		Unk_20(void);
	virtual void		Unk_21(void);
	virtual void		Unk_22(void);
};

class RefNiObject
{
	NiObject*	object;	// 00
};

// 018 (used to be 100, delta E8) confirmed, confirmed no virtual funcs
class NiObjectNET : public NiObject
{
public:
	NiObjectNET();
	~NiObjectNET();

#if RUNTIME
	MEMBER_FN_PREFIX(NiObjectNET);
	DEFINE_MEMBER_FN(GetExtraData, NiExtraData*, kNiObjectNET_GetExtraData, const char* name);
#endif

	NiFixedString m_pcName;						// 008 - name known
	NiTimeController	* m_controller;					// 00C - size ok

	// doesn't appear to be part of a class?
	NiExtraData			** m_ppkExtra;				// 010 - size ok
	UInt16				m_usExtraDataSize;				// 014 - size ok
	UInt16				m_usMaxSize;		// 016 - size ok
	// 018

	void SetName(const char* newName);

	bool AddExtraData(NiExtraData* extraData)
	{
		return ThisStdCall<bool>(0xA5BCA0, this, extraData);
	}

	bool RemoveExtraData(const NiFixedString& arKey) {
		return ThisStdCall<bool>(0xA5BE90, this, &arKey);
	}

	NiExtraData* GetExtraData(const NiFixedString& arKey) const
	{
		return ThisStdCall<NiExtraData*>(0xA5BDD0, this, &arKey);
	}
};
STATIC_ASSERT(sizeof(NiObjectNET) == 0x18);

// 030
class NiTexture : public NiObjectNET
{
public:
	NiTexture();
	~NiTexture();

	virtual UInt32	GetWidth(void) = 0;
	virtual UInt32	GetHeight(void) = 0;

	// 8
	struct Str028
	{
		UInt32	unk0;
		UInt32	unk4;
	};

	class RendererData
	{
	public:
		virtual void	Destroy(bool arg);
		virtual UInt32	GetWidth(void);
		virtual UInt32	GetHeight(void);
		virtual void	Unk_03(void);
	};

	enum
	{
		kPixelLayout_Palette8BPP = 0,
		kPixelLayout_Raw16BPP,
		kPixelLayout_Raw32BPP,
		kPixelLayout_Compressed,
		kPixelLayout_Bumpmap,
		kPixelLayout_Palette4BPP,
		kPixelLayout_Default,
	};

	enum
	{
		kMipMap_Disabled = 0,
		kMipMap_Enabled,
		kMipMap_Default,
	};

	enum
	{
		kAlpha_None = 0,
		kAlpha_Binary,	// 1bpp
		kAlpha_Smooth,	// 8bpp
		kAlpha_Default,
	};

	UInt32			pixelLayout;	// 018
	UInt32			alphaFormat;	// 01C
	UInt32			mipmapFormat;	// 020
	RendererData	* rendererData;	// 024
	NiTexture		* nextTex;		// 028 - linked list updated in ctor/dtor
	NiTexture		* prevTex;		// 02C
};

// NiDX9Direct3DTexture - not referenced

// 048
class NiSourceTexture : public NiTexture
{
public:
	NiSourceTexture();
	~NiSourceTexture();

	virtual void	Unk_15(void);
	virtual void	FreePixelData(void);
	virtual bool	Unk_17(void);

	UInt8		unk030;				// 030 - is static?
	UInt8		unk031[3];			// 031
	void		* unk034;			// 034
	const char	* fileName;			// 038
	NiObject	* pixelData;		// 03C - NiPixelData
	UInt8		loadDirectToRender;	// 040
	UInt8		persistRenderData;	// 041
	UInt8		pad042[2];			// 042
	void		* unk044;			// 044
};

// 04C
class NiSourceCubeMap : public NiSourceTexture
{
public:
	NiSourceCubeMap();
	~NiSourceCubeMap();

	UInt32	unk048;	// 048
};

// 040
class NiRenderedTexture : public NiTexture
{
public:
	NiRenderedTexture();
	~NiRenderedTexture();

	struct Str030
	{
		UInt32	pad00;
		UInt32	pad04;
		UInt32	width;
		UInt32	height;
	};

	virtual Str030 *	Unk_15(void);

	Str030	* unk030;	// 030
	UInt32	pad034;		// 034
	UInt32	pad038;		// 038
	UInt32	pad03C;		// 03C
};

// 05C
class NiRenderedCubeMap : public NiRenderedTexture
{
public:
	NiRenderedCubeMap();
	~NiRenderedCubeMap();

	UInt32		unk040;		// 040
	NiObject	* faces[6];	// 044
};

// 018
class NiSequenceStreamHelper : public NiObjectNET
{
public:
	NiSequenceStreamHelper();
	~NiSequenceStreamHelper();
};

//	name			d3dfmt   00 01 04       08       0C       10       14       18       1C 1D 20       24       28 29 2C       30       34 35 38       3C       40 41
//	R8G8B8			00000014 01 18 00000000 00000000 00000014 00000000 00000002 00000000 08 01 00000001 00000000 08 01 00000000 00000000 08 01 00000013 00000005 00 01
//	A8R8G8B8		00000015 01 20 00000001 00000000 00000015 00000000 00000002 00000000 08 01 00000001 00000000 08 01 00000000 00000000 08 01 00000003 00000000 08 01
//	X8R8G8B8		00000016 01 20 00000000 00000000 00000016 00000000 00000002 00000000 08 01 00000001 00000000 08 01 00000000 00000000 08 01 0000000E 00000005 08 01
//	R5G6B5			00000017 01 10 00000000 00000000 00000017 00000000 00000002 00000000 05 01 00000001 00000000 06 01 00000000 00000000 05 01 00000013 00000005 00 01
//	X1R5G5B5		00000018 01 10 00000000 00000000 00000018 00000000 00000002 00000000 05 01 00000001 00000000 05 01 00000000 00000000 05 01 0000000E 00000005 01 01
//	A1R5G5B5		00000019 01 10 00000001 00000000 00000019 00000000 00000002 00000000 05 01 00000001 00000000 05 01 00000000 00000000 05 01 00000003 00000000 01 01
//	A4R4G4B4		0000001A 01 10 00000001 00000000 0000001A 00000000 00000002 00000000 04 01 00000001 00000000 04 01 00000000 00000000 04 01 00000003 00000000 04 01
//	R3G3B2			0000001B 01 0A 00000000 00000000 0000001B 00000000 00000002 00000000 02 01 00000001 00000000 03 01 00000000 00000000 03 01 0000000E 00000005 02 01
//	A8				0000001C 01 08 0000000B 00000000 0000001C 00000000 00000003 00000000 08 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01
//	A8R3G3B2		0000001D 01 10 00000001 00000000 0000001D 00000000 00000002 00000000 02 01 00000001 00000000 03 01 00000000 00000000 03 01 00000003 00000000 08 01
//	X4R4G4B4		0000001E 01 10 00000000 00000000 0000001E 00000000 00000002 00000000 04 01 00000001 00000000 04 01 00000000 00000000 04 01 0000000E 00000000 04 01
//	A2B10G10R10		0000001F 01 20 00000001 00000000 0000001F 00000000 00000000 00000000 0A 01 00000001 00000000 0A 01 00000002 00000000 0A 01 00000003 00000000 02 01
//	A8B8G8R8		00000020 01 20 00000001 00000000 00000020 00000000 00000000 00000000 08 01 00000001 00000000 08 01 00000002 00000000 08 01 00000003 00000000 08 01
//	X8B8G8R8		00000021 01 20 00000000 00000000 00000021 00000000 00000000 00000000 08 01 00000001 00000000 08 01 00000002 00000000 08 01 0000000E 00000005 08 01
//	G16R16			00000022 01 20 0000000C 00000000 00000022 00000000 00000001 00000000 10 01 00000000 00000000 10 01 00000013 00000005 00 01 00000013 00000005 00 01
//	A2R10G10B10		00000023 01 20 00000001 00000000 00000023 00000000 00000002 00000000 0A 01 00000001 00000000 0A 01 00000000 00000000 0A 01 00000003 00000000 02 01
//	A16B16G16R16	00000024 01 40 00000001 00000000 00000024 00000000 00000000 00000001 10 01 00000001 00000001 10 01 00000002 00000001 10 01 00000003 00000001 10 01
//	A8P8			00000028 01 10 0000000C 00000000 00000028 00000000 00000010 00000003 08 01 00000003 00000000 08 01 00000013 00000005 00 01 00000013 00000005 00 01
//	P8				00000029 01 08 00000002 00000000 00000029 00000000 00000010 00000003 08 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01
//	L8				00000032 01 08 0000000B 00000000 00000032 00000000 00000009 00000000 08 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01
//	A8L8			00000033 01 10 0000000C 00000000 00000033 00000000 00000009 00000000 08 01 00000003 00000000 08 01 00000013 00000005 00 01 00000013 00000005 00 01
//	A4L4			00000034 01 08 0000000C 00000000 00000034 00000000 00000009 00000000 04 01 00000003 00000000 04 01 00000013 00000005 00 01 00000013 00000005 00 01
//	V8U8			0000003C 01 10 00000008 00000000 0000003C 00000000 00000005 00000000 08 01 00000006 00000000 08 01 00000013 00000005 00 01 00000013 00000005 00 01
//	L6V5U5			0000003D 01 10 00000009 00000000 0000003D 00000000 00000005 00000000 05 01 00000006 00000000 05 01 00000009 00000000 06 00 00000013 00000005 00 00
//	X8L8V8U8		0000003E 01 20 00000009 00000000 0000003E 00000000 00000005 00000000 08 01 00000006 00000000 08 01 00000009 00000000 08 00 0000000E 00000005 08 00
//	Q8W8V8U8		0000003F 01 20 00000008 00000000 0000003F 00000000 00000005 00000000 08 01 00000006 00000000 08 01 00000007 00000000 08 01 00000008 00000000 08 01
//	V16U16			00000040 01 20 00000008 00000000 00000040 00000000 00000005 00000000 10 01 00000006 00000000 10 01 00000013 00000005 00 01 00000013 00000005 00 01
//	A2W10V10U10		00000043 01 20 00000008 00000000 00000043 00000000 00000005 00000000 0A 01 00000006 00000000 0B 01 00000007 00000000 0B 01 00000013 00000005 00 01
//	D16_LOCKABLE	00000046 01 10 0000000F 00000000 00000046 00000000 00000011 00000000 10 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01
//	D32				00000047 01 20 0000000F 00000000 00000047 00000000 00000011 00000000 20 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01
//	D15S1			00000049 01 10 0000000F 00000000 00000049 00000000 00000012 00000000 01 01 00000011 00000000 0F 01 00000013 00000005 00 01 00000013 00000005 00 01
//	D24S8			0000004B 01 20 0000000F 00000000 0000004B 00000000 00000012 00000000 08 01 00000011 00000000 18 01 00000013 00000005 00 01 00000013 00000005 00 01
//	D24X8			0000004D 01 20 0000000F 00000000 0000004D 00000000 0000000E 00000000 08 01 00000011 00000000 18 01 00000013 00000005 00 01 00000013 00000005 00 01
//	D24X4S4			0000004F 01 20 0000000F 00000000 0000004F 00000000 00000012 00000000 04 01 0000000E 00000000 04 01 00000011 00000000 18 01 00000013 00000005 00 01
//	D16				00000050 01 10 0000000F 00000000 00000050 00000000 00000011 00000000 10 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01
//	L16				00000051 01 10 0000000B 00000000 00000051 00000000 00000009 00000000 10 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01
//	D32F_LOCKABLE	00000052 01 20 0000000B 00000000 00000052 00000000 0000000E 00000005 20 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01
//	D24FS8			00000053 01 20 0000000B 00000000 00000053 00000000 0000000E 00000005 20 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01
//	Q16W16V16U16	0000006E 01 40 0000000B 00000000 0000006E 00000000 0000000E 00000005 40 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01
//	R16F			0000006F 01 10 0000000B 00000000 0000006F 00000000 00000000 00000001 10 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01
//	G16R16F			00000070 01 20 0000000C 00000000 00000070 00000000 00000000 00000001 10 01 00000001 00000001 10 01 00000013 00000005 00 01 00000013 00000005 00 01
//	A16B16G16R16F	00000071 01 40 00000001 00000000 00000071 00000000 00000000 00000001 10 01 00000001 00000001 10 01 00000002 00000001 10 01 00000003 00000001 10 01
//	R32F			00000072 01 20 0000000B 00000000 00000072 00000000 00000000 00000002 20 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01
//	G32R32F			00000073 01 40 0000000C 00000000 00000073 00000000 00000000 00000002 20 01 00000001 00000002 20 01 00000013 00000005 00 01 00000013 00000005 00 01
//	A32B32G32R32F	00000074 01 80 00000001 00000000 00000074 00000000 00000000 00000002 20 01 00000001 00000002 20 01 00000002 00000002 20 01 00000003 00000002 20 01
//	CxV8U8			00000075 01 10 0000000B 00000000 00000075 00000000 0000000E 00000005 10 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01
//	DXT1			xxxxxxxx 01 00 00000004 00000000 xxxxxxxx 00000000 00000004 00000004 00 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01
//	DXT3			xxxxxxxx 01 00 00000005 00000000 xxxxxxxx 00000000 00000004 00000004 00 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01
//	DXT5			xxxxxxxx 01 00 00000006 00000000 xxxxxxxx 00000000 00000004 00000004 00 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01

//	invalid			xxxxxxxx 01 00 0000000B 00000000 xxxxxxxx 00000000 0000000E 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01
//	D32_LOCKABLE	00000054 01 00 0000000B 00000000 00000054 00000000 0000000E 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01
//	S8_LOCKABLE		00000055 01 00 0000000B 00000000 00000055 00000000 0000000E 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01
//	VERTEXDATA		00000064 01 00 0000000B 00000000 00000064 00000000 0000000E 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01
//	INDEX16			00000065 01 00 0000000B 00000000 00000065 00000000 0000000E 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01
//	INDEX32			00000066 01 00 0000000B 00000000 00000066 00000000 0000000E 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01 00000013 00000005 00 01

// 44
struct TextureFormat
{
	enum
	{
		kFormat_RGB = 0,		// 0
		kFormat_RGBA,			// 1
		kFormat_A,				// 2
		kFormat_Unk3,			// 3
		kFormat_DXT1,			// 4
		kFormat_DXT3,			// 5
		kFormat_DXT5,			// 6
		kFormat_Unk7,			// 7
		kFormat_Bump,			// 8
		kFormat_BumpLuminance,	// 9
		kFormat_UnkA,			// A
		kFormat_Other,			// B - A8 L8 L16 D32F_LOCKABLE D24FS8 Q16W16V16U16 R16F R32F CxV8U8
		kFormat_Other2,			// C - G16R16 A8P8 A8L8 A4L4 G16R16F G32R32F
		kFormat_UnkD,			// D
		kFormat_UnkE,			// E
		kFormat_Depth,			// F
	};

	enum
	{
		kType_Blue,			// 00
		kType_Green,		// 01
		kType_Red,			// 02
		kType_Alpha,		// 03
		kType_Unk04,		// 04
		kType_BumpU,		// 05
		kType_BumpV,		// 06
		kType_Unk07,		// 07
		kType_Unk08,		// 08
		kType_Luminance,	// 09
		kType_Unk0A,		// 0A
		kType_Unk0B,		// 0B
		kType_Unk0C,		// 0C
		kType_Unk0D,		// 0D
		kType_Unused,		// 0E
		kType_Unk0F,		// 0F
		kType_PalIdx,		// 10
		kType_Depth,		// 11
		kType_Stencil,		// 12
		kType_None,			// 13
	};

	enum
	{
		kType2_Default,		// 00
		kType2_16Bit,		// 01
		kType2_32Bit,		// 02
		kType2_Palettized,	// 03
		kType2_Compressed,	// 04
		kType2_None,		// 05
	};

	UInt8	unk00;		// 00 - always 01? (checked all D3DFMT)
	UInt8	bpp;		// 01 - zero for dxt
	UInt8	pad02[2];	// 02
	UInt32	format;		// 04 - default kFormat_A (really)
	UInt32	unk08;		// 08 - always 00000000? (checked all D3DFMT)
	UInt32	d3dfmt;		// 0C
	UInt32	unk10;		// 10 - always 00000000? (checked all D3DFMT)

	struct Channel
	{
		UInt32	type;		// 0
		UInt32	type2;		// 4
		UInt8	bits;		// 8
		UInt8	unk9;		// 9 - only seen non-01 when unused (L6V5U5 X8L8V8U8)
		UInt8	padA[2];	// A
	};

	Channel	channels[4];	// 14

	void InitFromD3DFMT(UInt32 fmt);
};

// 070
class NiPixelData : public NiObject
{
public:
	NiPixelData();
	~NiPixelData();

	// face size = unk05C[mipmapLevels]
	// total size = face size * numFaces

	TextureFormat	format;		// 008
	NiRefObject		* unk04C;	// 04C
	UInt32	unk050;			// 050
	UInt32	* width;		// 054 - array for mipmaps?
	UInt32	* height;		// 058
	UInt32	* unk05C;		// 05C - sizes?
	UInt32	mipmapLevels;	// 060
	UInt32	unk064;			// 064
	UInt32	unk068;			// 068
	UInt32	numFaces;		// 06C
};

class ControlledBlock;
struct NiQuatTransform;

// 0C
class NiInterpolator : public NiObject
{
public:
	NiInterpolator();
	~NiInterpolator();

	virtual bool	Update(float fTime, NiObjectNET* pkInterpTarget, NiQuatTransform& kValue);
	virtual void	Unk_24(void);
	virtual void	Unk_25(void);
	virtual void	Unk_26(void);
	virtual void	Unk_27(void);
	virtual void	Unk_28(void);
	virtual void	Unk_29(void);
	virtual void	Unk_2A(void);
	virtual void	Unk_2B(void);
	virtual void	Unk_2C(void);
	virtual void	Unk_2D(void);
	virtual void	Unk_2E(void);
	virtual void	Unk_2F(void);
	virtual void	Unk_30(void);
	virtual void	Unk_31(void);
	virtual void	Unk_32(void);
	virtual void	Unk_33(void);
	virtual void	Unk_34(void);
	virtual void	Unk_35(void);
	virtual void	Unk_36(void);

	float m_fLastTime;

	bool TimeHasChanged(float fTime)
	{
		return (m_fLastTime != fTime);
	}
};

struct NiKeyBasedInterpolator : NiInterpolator
{
};

struct NiAnimationKey
{
	
	/* 1224 */
	enum KeyType
	{
		NOINTERP = 0x0,
		LINKEY = 0x1,
		BEZKEY = 0x2,
		TCBKEY = 0x3,
		EULERKEY = 0x4,
		STEPKEY = 0x5,
		NUMKEYTYPES = 0x6,
	};

	float m_fTime;

	typedef void (*InterpFunction)(float fTime, const NiAnimationKey* pKey0,
		const NiAnimationKey* pKey1, void* pResult);


	float GetTime() const
	{
		return m_fTime;
	}

	enum KeyContent
	{
		FLOATKEY,
		POSKEY,
		ROTKEY,
		COLORKEY,
		TEXTKEY,
		BOOLKEY,
		NUMKEYCONTENTS
	};

	NiAnimationKey* GetKeyAt(unsigned int uiIndex, unsigned char ucKeySize) const
	{
		return (NiAnimationKey*) ((char*) this + uiIndex * ucKeySize);
	}
};

struct NiRotKey : NiAnimationKey
{
	NiQuaternion m_quat;

	const NiQuaternion& GetQuaternion() const
	{
		return m_quat;
	}

	NiRotKey* GetKeyAt(unsigned int uiIndex, unsigned char ucKeySize)
	{
		return (NiRotKey*) NiAnimationKey::GetKeyAt(uiIndex, ucKeySize);
	}

	static InterpFunction GetInterpFunction(KeyType eType)
	{
		const auto* interps = reinterpret_cast<InterpFunction*>(0x11F3CC0);
		return interps[eType];
	}

	static NiQuaternion GenInterp(float fTime, NiRotKey* pkKeys, KeyType eType, unsigned int uiNumKeys, unsigned short& uiLastIdx,unsigned char ucSize)
	{
	    NIASSERT(uiNumKeys != 0);
	    if (uiNumKeys == 1)
	    {
	        if (eType != EULERKEY)
	            return pkKeys->GetKeyAt(0, ucSize)->GetQuaternion();
	    }

	    // If rotation keys are specified as Euler angles then execution is
	    // directed to another routine due to the special requirements in
	    // handling Euler angles.
	    if ( eType == EULERKEY )
	    {
	        InterpFunction interp = GetInterpFunction(eType);
	        NIASSERT( interp );
	        NiQuaternion kQuat;
	        interp(fTime, pkKeys->GetKeyAt(0, ucSize), 0, &kQuat);
	        return kQuat;
	    }

	    unsigned int uiNumKeysM1 = uiNumKeys - 1;

	    // This code assumes that the time values in the keys are ordered by
	    // increasing value.  The search can therefore begin at uiLastIdx rather
	    // than zero each time.  The idea is to provide an O(1) lookup based on
	    // time coherency of the keys.

	    float fLastTime = pkKeys->GetKeyAt(uiLastIdx, ucSize)->GetTime();
	    if ( fTime < fLastTime )
	    {
	        uiLastIdx = 0;
	        fLastTime = pkKeys->GetKeyAt(0, ucSize)->GetTime();
	    }
	    
	    unsigned int uiNextIdx;
	    float fNextTime = 0.0f;
	    for (uiNextIdx = uiLastIdx + 1; uiNextIdx <= uiNumKeysM1; uiNextIdx++)
	    {
	        fNextTime = pkKeys->GetKeyAt(uiNextIdx, ucSize)->GetTime();
	        if ( fTime <= fNextTime )
	            break;

	        uiLastIdx++;
	        fLastTime = fNextTime;
	    }

	    NIASSERT(uiNextIdx < uiNumKeys);

	    // interpolate the keys, requires that the time is normalized to [0,1]
	    float fNormTime = (fTime - fLastTime)/(fNextTime - fLastTime);
	    NiRotKey::InterpFunction interp = NiRotKey::GetInterpFunction(eType);
	    NIASSERT( interp );
	    NiQuaternion kQuat;
	    interp(fNormTime, pkKeys->GetKeyAt(uiLastIdx, ucSize),
	        pkKeys->GetKeyAt(uiNextIdx, ucSize), &kQuat);
	    return kQuat;
	}
};

struct NiTCBRotKey : NiRotKey
{
	float m_fTension;
	float m_fContinuity;
	float m_fBias;
	NiQuaternion m_A;
	NiQuaternion m_B;
};

struct NiEulerRotKey : NiRotKey
{
	unsigned int m_uiNumKeys[3];
	KeyType m_eType[3];
	char m_ucSizes[3];
	NiFloatKey *m_apkKeys[3];
	unsigned int m_uiLastIdx[3];
};

struct NiBezRotKey : NiRotKey
{
	NiQuaternion m_IntQuat;
};

struct NiPosKey : NiAnimationKey
{
	NiPoint3 m_Pos;

	NiPosKey* GetKeyAt(unsigned int uiIndex, unsigned char ucKeySize)
	{
		return (NiPosKey*) NiAnimationKey::GetKeyAt(uiIndex, ucKeySize);
	}

	const NiPoint3& GetPos() const
	{
		return m_Pos;
	}
	
	static InterpFunction GetInterpFunction(KeyType eType)
	{
		const auto* interps = reinterpret_cast<InterpFunction*>(0x11F3CA8);
		return interps[eType];
	}

	static NiPoint3 GenInterp(float fTime, NiPosKey* pkKeys, KeyType eType, unsigned int uiNumKeys, unsigned short& uiLastIdx, unsigned char ucSize)
	{
		ASSERT(uiNumKeys != 0);
		if (uiNumKeys == 1)
			return pkKeys->GetKeyAt(0, ucSize)->GetPos();
		
		unsigned int uiNumKeysM1 = uiNumKeys - 1;

		// This code assumes that the time values in the keys are ordered by
		// increasing value.  The search can therefore begin at uiLastIdx rather
		// than zero each time.  The idea is to provide an O(1) lookup based on
		// time coherency of the keys.

		float fLastTime = pkKeys->GetKeyAt(uiLastIdx, ucSize)->GetTime();
		if ( fTime < fLastTime )
		{
			uiLastIdx = 0;
			fLastTime = pkKeys->GetKeyAt(0, ucSize)->GetTime();
		}
    
		unsigned int uiNextIdx;
		float fNextTime = 0.0f;
		for (uiNextIdx = uiLastIdx + 1; uiNextIdx <= uiNumKeysM1; uiNextIdx++)
		{
			fNextTime = pkKeys->GetKeyAt(uiNextIdx, ucSize)->GetTime();
			if ( fTime <= fNextTime )
				break;

			uiLastIdx++;
			fLastTime = fNextTime;
		}

		ASSERT(uiNextIdx < uiNumKeys);

		// interpolate the keys, requires that the time is normalized to [0,1]
		float fNormTime = (fTime - fLastTime)/(fNextTime - fLastTime);
		InterpFunction interp = GetInterpFunction(eType);
		ASSERT( interp );
		NiPoint3 kResult;
		interp(fNormTime, pkKeys->GetKeyAt(uiLastIdx, ucSize),
			pkKeys->GetKeyAt(uiNextIdx, ucSize), &kResult);
		return kResult;
	}

};

struct NiBezPosKey : NiPosKey
{
	NiPoint3 m_InTan;
	NiPoint3 m_OutTan;
	NiPoint3 m_A;
	NiPoint3 m_B;
};

struct NiTCBPosKey : NiPosKey
{
	float m_fTension;
	float m_fContinuity;
	float m_fBias;
	NiPoint3 m_DS;
	NiPoint3 m_DD;
	NiPoint3 m_A;
	NiPoint3 m_B;
};

struct NiFloatKey : NiAnimationKey
{
	float m_fValue;

	float GetValue() const
	{
		return m_fValue;
	}

	NiFloatKey* GetKeyAt(unsigned int uiIndex, unsigned char ucKeySize)
	{
		return (NiFloatKey*) NiAnimationKey::GetKeyAt(uiIndex, ucKeySize);
	}

	static InterpFunction GetInterpFunction(KeyType eType)
	{
		const auto* interps = (InterpFunction*)0x11F3C90;
		return interps[eType];
	}


	static float GenInterp(float fTime, NiFloatKey* pkKeys, KeyType eType, unsigned int uiNumKeys, unsigned short& uiLastIdx, unsigned char ucSize)
	{
		NIASSERT(uiNumKeys != 0);
		if (uiNumKeys == 1)
			return pkKeys->GetKeyAt(0, ucSize)->GetValue();

		if (fTime == -NI_INFINITY)
			return pkKeys->GetKeyAt(0, ucSize)->GetValue();

		unsigned int uiNumKeysM1 = uiNumKeys - 1;

		// This code assumes that the time values in the keys are ordered by
		// increasing value.  The search can therefore begin at uiLastIdx rather
		// than zero each time.  The idea is to provide an O(1) lookup based on
		// time coherency of the keys.

		// Copy the last index to a stack variable here to ensure that each thread
		// has its own consistent copy of the value. The stack variable is copied
		// back to the reference variable at the end of this function.
		unsigned int uiStackLastIdx = uiLastIdx;

		float fLastTime = pkKeys->GetKeyAt(uiStackLastIdx, ucSize)->GetTime();
		if(fTime < fLastTime)
		{
			uiStackLastIdx = 0;
		}

		unsigned int uiNextIdx;
		for (uiNextIdx = uiStackLastIdx + 1; uiNextIdx <= uiNumKeysM1; uiNextIdx++)
		{
			float fNextTime = pkKeys->GetKeyAt(uiNextIdx, ucSize)->GetTime();
			if(fTime <= fNextTime)
				break;

			uiStackLastIdx++;
		}
    
		NIASSERT(uiNextIdx < uiNumKeys);

		// interpolate the keys, requires that the time is normalized to [0,1]
		InterpFunction interp = GetInterpFunction(eType);
		NIASSERT(interp);
		float fReturn;
		interp(uiNumKeys, pkKeys->GetKeyAt(uiStackLastIdx, ucSize), pkKeys->GetKeyAt(uiNextIdx, ucSize), &fReturn);
		uiLastIdx = uiStackLastIdx;
		return fReturn;
	}

};

struct NiBezFloatKey : NiFloatKey
{
	float m_fInTan;
	float m_fOutTan;
};

struct NiTCBFloatKey : NiFloatKey
{
	float m_fTension;
	float m_fContinuity;
	float m_fBias;
	float m_fDS;
	float m_fDD;
};

class NiTransformData : public NiObject
{
public:
	UInt16 m_uiNumRotKeys;
	UInt16 m_uiNumPosKeys;
	UInt16 m_uiNumScaleKeys;
	NiAnimationKey::KeyType m_eRotType;
	NiAnimationKey::KeyType m_ePosType;
	NiAnimationKey::KeyType m_eScaleType;

	UInt8 m_ucRotSize;
	UInt8 m_ucPosSize;
	UInt8 m_ucScaleSize;
	NiRotKey* m_pkRotKeys;
	NiPosKey* m_pkPosKeys;
	NiFloatKey* m_pkScaleKeys;

	template <typename T>
	std::span<T> GetRotKeys() const
	{
		return { reinterpret_cast<T*>(m_pkRotKeys), m_uiNumRotKeys };
	}

	template <typename T>
	std::span<T> GetPosKeys() const
	{
		return { reinterpret_cast<T*>(m_pkPosKeys), m_uiNumPosKeys };
	}

	template <typename T>
	std::span<T> GetScaleKeys() const
	{
		return {reinterpret_cast<T*>(m_pkScaleKeys), m_uiNumScaleKeys };
	}

	NiPosKey* GetPosAnim(unsigned int& iNumKeys, NiAnimationKey::KeyType& eType, unsigned char& ucSize) const
	{
		iNumKeys = m_uiNumPosKeys;
		eType = m_ePosType;
		ucSize = m_ucPosSize;
		return m_pkPosKeys;
	}

	NiRotKey* GetRotAnim(unsigned int& uiNumKeys, NiAnimationKey::KeyType& eType, unsigned char& ucSize) const
	{
		uiNumKeys = m_uiNumRotKeys;
		eType = m_eRotType;
		ucSize = m_ucRotSize;
		return m_pkRotKeys;
	}

	NiFloatKey* GetScaleAnim(unsigned int& uiNumKeys, NiFloatKey::KeyType& eType, unsigned char& ucSize) const
	{
		uiNumKeys = m_uiNumScaleKeys;
		eType = m_eScaleType;
		ucSize = m_ucScaleSize;
		return m_pkScaleKeys;
	}
};

struct NiQuatTransform
{
	static inline NiPoint3 INVALID_TRANSLATE = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	static inline NiQuaternion INVALID_ROTATE = { -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX };
	static inline float INVALID_SCALE = -FLT_MAX;
	
	NiPoint3 m_kTranslate;
	NiQuaternion m_kRotate;
	float m_fScale;

	NiQuatTransform(): m_kTranslate(INVALID_TRANSLATE), m_kRotate(INVALID_ROTATE), m_fScale(INVALID_SCALE) {}
	NiQuatTransform(const NiPoint3& translate, const NiQuaternion& rotate, float scale)
		: m_kTranslate(translate), m_kRotate(rotate), m_fScale(scale) {}

	void MakeInvalid()
	{
		SetTranslateValid(false);
		SetRotateValid(false);
		SetScaleValid(false);
	}

	bool IsScaleValid() const
	{
		return m_fScale != -NI_INFINITY;
	}

	bool IsRotateValid() const
	{
		return m_kRotate.m_fX != -NI_INFINITY;
	}

	bool IsTranslateValid() const
	{
		return m_kTranslate.x != -NI_INFINITY;
	}

	bool IsTransformInvalid() const
	{
		return !(IsScaleValid() || IsRotateValid() || IsTranslateValid());
	}

	void SetScaleValid(bool bValid)
	{
		if (!bValid)
			m_fScale = -NI_INFINITY;
	}

	void SetRotateValid(bool bValid)
	{
		if (!bValid)
			m_kRotate.m_fX = -NI_INFINITY;
	}

	void SetTranslateValid(bool bValid)
	{
		if (!bValid)
			m_kTranslate.x = -NI_INFINITY;
	}

	void SetScale(float fScale)
	{
		m_fScale = fScale;
		SetScaleValid(true);
	}

	float GetScale() const
	{
		return m_fScale;
	}

	const NiQuaternion& GetRotate() const
	{
		return m_kRotate;
	}

	void SetRotate(const NiQuaternion& kRotate)
	{
		m_kRotate = kRotate;
		SetRotateValid(true);
	}

	void SetRotate(const NiMatrix3& kRotate)
	{
		NiQuaternion kRotateQuat;
		kRotateQuat.FromRotation(kRotate);
		SetRotate(kRotateQuat);
	}

	void SetTranslate(const NiPoint3& kTranslate)
	{
		m_kTranslate = kTranslate;
		SetTranslateValid(true);
	}

	const NiPoint3& GetTranslate() const
	{
		return m_kTranslate;
	}

	NiQuatTransform operator*(const NiQuatTransform& kTransform) const
	{
		NiQuatTransform kDest;

		if (!IsScaleValid() || !kTransform.IsScaleValid())
		{
			kDest.SetScaleValid(false);
		}
		else
		{
			kDest.SetScale(m_fScale * kTransform.GetScale());
		}

		if (!IsRotateValid() || !kTransform.IsRotateValid())
		{
			kDest.SetRotateValid(false);
		}
		else
		{
			NiQuaternion kTempRot = m_kRotate * kTransform.GetRotate();
			kTempRot.Normalize();
			kDest.SetRotate(kTempRot);
		}

		if (!IsTranslateValid() || !kTransform.IsTranslateValid())
		{
			kDest.SetTranslateValid(false);
		}
		else
		{
			kDest.SetTranslate(m_kTranslate + kTransform.GetTranslate());
		}

		return kDest;
	}

	NiQuatTransform operator*(float fValue) const
	{
		NiQuatTransform kDest = *this;
		if (kDest.IsTranslateValid())
			kDest.SetTranslate(kDest.GetTranslate() * fValue);
		if (kDest.IsRotateValid())
			kDest.SetRotate(kDest.GetRotate() * fValue);
		if (kDest.IsScaleValid())
			kDest.SetScale(kDest.GetScale() * fValue);
		return kDest;
	}

	NiQuatTransform operator-(const NiQuatTransform& kTransform) const
	{
		NiQuatTransform kDest{};
		if (kTransform.IsTranslateValid() && IsTranslateValid())
			kDest.SetTranslate(m_kTranslate - kTransform.GetTranslate());
		else
			kDest.SetTranslateValid(false);

		if (kTransform.IsRotateValid() && IsRotateValid())
			kDest.SetRotate(m_kRotate * kTransform.GetRotate().Inverse());
		else
			kDest.SetRotateValid(false);
		
		if (kTransform.IsScaleValid() && IsScaleValid())
			kDest.SetScale(m_fScale - kTransform.GetScale());
		else
			kDest.SetScaleValid(false);
		return kDest;
	}
};

class NiTransformInterpolator : public NiKeyBasedInterpolator {
public:
	NiQuatTransform		m_kTransformValue;
	NiPointer<NiTransformData> m_spData;
	UInt16				m_usLastTransIdx;
	UInt16				m_usLastRotIdx;
	UInt16				m_usLastScaleIdx;
	NiPoint3			m_kDefaultTranslate;
	bool				bPose;
	
	NiFloatKey* GetScaleData(unsigned int& uiNumKeys, NiFloatKey::KeyType& eType, unsigned char& ucSize) const
	{
		if (m_spData)
		{
			return m_spData->GetScaleAnim(uiNumKeys, eType, ucSize);
		}

		uiNumKeys = 0;
		eType = NiFloatKey::NOINTERP;
		ucSize = 0;
		return NULL;
	}
	
	NiPosKey* GetPosData(
		unsigned int& uiNumKeys, NiAnimationKey::KeyType& eType, unsigned char& ucSize) const
	{
		if (m_spData)
		{
			return m_spData->GetPosAnim(uiNumKeys, eType, ucSize);
		}

		uiNumKeys = 0;
		eType = NiAnimationKey::NOINTERP;
		ucSize = 0;
		return nullptr;
	}

	NiRotKey* GetRotData(unsigned int& uiNumKeys, NiAnimationKey::KeyType& eType, unsigned char& ucSize) const
	{
		if (m_spData)
		{
			return m_spData->GetRotAnim(uiNumKeys, eType, ucSize);
		}

		uiNumKeys = 0;
		eType = NiAnimationKey::NOINTERP;
		ucSize = 0;
		return nullptr;
	}

	std::optional<NiQuatTransform> GetTransformAt(unsigned int uiIndex) const
	{
		unsigned int numKeys;
		NiAnimationKey::KeyType keyType;
		unsigned char keySize;
		
		auto* posData = GetPosData(numKeys, keyType, keySize);
		NiPoint3 pos;
		if (uiIndex >= numKeys)
			pos = NiPoint3::INVALID_POINT;
		else
			pos = posData->GetKeyAt(uiIndex, keySize)->m_Pos;
		
		auto* rotData = GetRotData(numKeys, keyType, keySize);
		NiQuaternion rot;
		if (uiIndex >= numKeys)
			rot = NiQuaternion::INVALID_QUATERNION;
		else
			rot = rotData->GetKeyAt(uiIndex, keySize)->GetQuaternion();

		float scale;
		auto* scaleData = GetScaleData(numKeys, keyType, keySize);
		if (uiIndex >= numKeys)
			scale = -NI_INFINITY;
		else
			scale = scaleData->GetKeyAt(uiIndex, keySize)->m_fValue;
		
		return NiQuatTransform(pos, rot, scale);
	}

	bool _Update(float fTime, NiObjectNET* pkInterpTarget, NiQuatTransform& kValue);

	void Pause()
	{
		this->bPose = true;
	}

	static NiTransformInterpolator* Create(const NiQuatTransform& kTransform)
	{
		auto* memory = NiNew<NiTransformInterpolator>();
		return ThisStdCall<NiTransformInterpolator*>(0xA3FAE0, memory, kTransform);
	}
};
static_assert(sizeof(NiTransformInterpolator) == 0x48);

struct NiUpdateData
{
	float fTime;
	unsigned int RenderUseDepth;
	bool bParentsChecked;
	NiCamera *pCamera;
	unsigned int Flags;
	unsigned int RenderObjects;
	unsigned int FadeNodeDepth;
};

/* 12659 */
class NiTimeController : public NiObject
{
public:

	virtual void *Unk_23();
	virtual void *Unk_24();
	virtual void *Update(NiUpdateData *);
	virtual void *SetTarget(NiNode *);
	virtual void *Unk_27();
	virtual void *Unk_28();
	virtual void *Unk_29();
	virtual void *OnPreDisplay();
	virtual void *Unk_2B();
	virtual void *Unk_2C();
	
	enum
	{
		ANIMTYPE_MASK           = 0x0001,
		ANIMTYPE_POS            = 0,
		CYCLETYPE_MASK          = 0x0006,
		CYCLETYPE_POS           = 1,
		ACTIVE_MASK             = 0x0008,
		DIRECTION_MASK          = 0x0010,
		MANAGERCONTROLLED_MASK  = 0x0020,
		COMPUTESCALEDTIME_MASK  = 0x0040,
		FORCEUDPATE_MASK        = 0x0080
	};

	enum CycleType
	{
		LOOP,
		REVERSE,
		CLAMP,
		MAX_CYCLE_TYPES
	};
	
	UInt16 m_uFlags;
	float m_fFrequency;
	float m_fPhase;
	float m_fLoKeyTime;
	float m_fHiKeyTime;
	float m_fStartTime;
	float m_fLastTime;
	float m_fWeightedLastTime;
	float m_fScaledTime;
	NiObjectNET* m_pkTarget;
	NiPointer<NiTimeController> m_spNext;

	bool GetActive() const
	{
		return GetBit(ACTIVE_MASK);
	}
};
static_assert(sizeof(NiTimeController) == 0x34);

class NiGlobalStringTable : public NiMemObject {
public:
	typedef char* GlobalStringHandle;

	NiTArray<GlobalStringHandle>		m_kHashArray[512];
	void*									unk2000[32];
	NiCriticalSection						m_kCriticalSection;
	void*									unk20A0[24];

	static GlobalStringHandle AddString(const char* pcString)
	{
		return CdeclCall<GlobalStringHandle>(0xA5B690, pcString);
	}

	static void IncRefCount(GlobalStringHandle arHandle)
	{
		if (!arHandle)
			return;

		InterlockedIncrement(reinterpret_cast<size_t*>(GetRealBufferStart(arHandle)));
	}
	
	static void DecRefCount(GlobalStringHandle arHandle)
	{
		if (!arHandle)
			return;

		InterlockedDecrement(reinterpret_cast<size_t*>(GetRealBufferStart(arHandle)));
	}
	
	static UInt32 GetLength(const GlobalStringHandle& arHandle)
	{
		if (!arHandle)
			return 0;

		return GetRealBufferStart(arHandle)[1];
	}

	static char* GetRealBufferStart(const GlobalStringHandle& arHandle)
	{
		return static_cast<char*>(arHandle) - 2 * sizeof(size_t);
	}
};
ASSERT_SIZE(NiGlobalStringTable, 0x2100);


class NiInterpController;
class NiBlendInterpolator;

// 068
class NiControllerSequence : public NiObject
{
public:
	NiControllerSequence();
	~NiControllerSequence();
	void AttachInterpolatorsAdditive(char cPriority) const;
	void DetachInterpolators() const;
	void DetachInterpolatorsHooked() const;
	void DetachInterpolatorsAdditive() const;
	void RemoveInterpolator(const NiFixedString& name) const;
	void RemoveInterpolator(unsigned int index) const;
	float GetEaseSpinner() const;

	
	static NiControllerSequence* Create()
	{
		return CdeclCall<NiControllerSequence*>(0xA326C0);
	}

	static NiControllerSequence* Create(const NiFixedString &kName, unsigned int uiArraySize, unsigned int uiArrayGrowBy)
	{
		auto* memory = NiNew<NiControllerSequence>();
		return ThisStdCall<NiControllerSequence*>(0xA32A10, memory, &kName, uiArraySize, uiArrayGrowBy);
	}
	
	enum
	{
		kCycle_Loop = 0,
		kCycle_Reverse,
		kCycle_Clamp,
	};

	struct IDTag
	{
		NiFixedString m_kAVObjectName;
		NiFixedString m_kPropertyType;
		NiFixedString m_kCtlrType;
		NiFixedString m_kCtlrID;
		NiFixedString m_kInterpolatorID;

		void ClearValues()
		{
			m_kAVObjectName = nullptr;
			m_kPropertyType = nullptr;
			m_kCtlrType = nullptr;
			m_kCtlrID = nullptr;
			m_kInterpolatorID = nullptr;
		}
	};
	
	struct InterpArrayItem
	{
		NiPointer<NiInterpolator> m_spInterpolator;
		NiPointer<NiInterpController> m_spInterpCtlr;
		NiBlendInterpolator* m_pkBlendInterp;
		unsigned char m_ucBlendIdx;
		char m_ucPriority;
		UInt16 pad;

		void ClearValues()
		{
			m_spInterpolator = nullptr;
			m_spInterpCtlr = nullptr;
			m_pkBlendInterp = nullptr;
			m_ucBlendIdx = INVALID_INDEX;
		}

		IDTag& GetIDTag(const NiControllerSequence* owner) const
		{
			const auto index = GetIndex(owner);
#if _DEBUG
			assert(index < owner->m_uiArraySize);
#endif
			return owner->m_pkIDTagArray[index];
		}

		unsigned int GetIndex(const NiControllerSequence* owner) const
		{
			return this - owner->m_pkInterpArray;
		}
	};

	

	enum AnimState
	{
		INACTIVE,
		ANIMATING,
		EASEIN,
		EASEOUT,
		TRANSSOURCE,
		TRANSDEST,
		MORPHSOURCE
	};

	enum CycleType
	{
		LOOP = 0x0,
		REVERSE = 0x1,
		CLAMP = 0x2,
		MAX_CYCLE_TYPES = 0x3,
	};

	std::span<InterpArrayItem> GetControlledBlocks() const;
	std::span<IDTag> GetIDTags() const;

	NiFixedString m_kName; // 8
	UInt32 m_uiArraySize; // C
	UInt32 m_uiArrayGrowBy; // 10
	InterpArrayItem* m_pkInterpArray; // 14
	IDTag* m_pkIDTagArray; // 18
	float m_fSeqWeight; // 1C
	NiTextKeyExtraData* m_spTextKeys; // 20
	CycleType m_eCycleType;
	float m_fFrequency;
	float m_fBeginKeyTime;
	float m_fEndKeyTime;
	float m_fLastTime;
	float m_fWeightedLastTime;
	float m_fLastScaledTime;
	NiControllerManager* m_pkOwner;
	AnimState m_eState;
	float m_fOffset;
	float m_fStartTime;
	float m_fEndTime;
	float m_fDestFrame;
	NiControllerSequence* m_pkPartnerSequence;
	NiFixedString m_kAccumRootName;
	UInt32 m_pkAccumRoot;
	UInt32 m_spDeprecatedStringPalette; // deprecated string palette
	UInt16 usCurAnimNIdx;
	void* spAnimNotes;
	UInt16 usNumNotes;
	bool bRemovableObjects;

	InterpArrayItem* GetControlledBlock(const char* name) const;
	InterpArrayItem* GetControlledBlock(const NiFixedString& name) const;
	InterpArrayItem* GetControlledBlock(const NiInterpolator* interpolator) const;
	InterpArrayItem* GetControlledBlock(const NiBlendInterpolator* interpolator) const;

	virtual bool Deactivate(float fEaseOutTime, bool bTransition);
	bool Deactivate_(float fEaseOutTime, bool bTransition);
	bool DeactivateNoReset(float fEaseOutTime);
	
	void ResetSequence()
	{
		m_fOffset = -NI_INFINITY;
	}
	
	void AttachInterpolators(char cPriority);
	void AttachInterpolatorsHooked(char cPriority);

	bool Activate(char cPriority, bool bStartOver, float fWeight,
	              float fEaseInTime, NiControllerSequence* pkTimeSyncSeq,
	              bool bTransition);
	bool ActivateBlended(char cPriority, bool bStartOver, float fWeight,
				  float fEaseInTime, NiControllerSequence* pkTimeSyncSeq,
				  bool bTransition);
	bool ActivateNoReset(float fEaseInTime);

	NiControllerManager* GetOwner() const
	{
		return m_pkOwner;
	}

	AnimState GetState() const
	{
		return m_eState;
	}

	float ComputeScaledTime(float fTime, bool bStoreResult)
	{
		return ThisStdCall(0xA30970, this, fTime, bStoreResult);
	}

	bool StartBlend(NiControllerSequence* pkDestSequence, float fDuration,
		float fDestFrame, int iPriority, float fSourceWeight,
		float fDestWeight, NiControllerSequence* pkTimeSyncSeq);
	bool StartMorph(NiControllerSequence* pkDestSequence,
	                float fDuration, int iPriority, float fSourceWeight, float fDestWeight);

	bool VerifyDependencies(NiControllerSequence *pkSequence) const;
	bool VerifyMatchingMorphKeys(NiControllerSequence *pkTimeSyncSeq) const;
	bool CanSyncTo(NiControllerSequence *pkTargetSequence) const;
	void SetTimePassed(float fTime, bool bUpdateInterpolators);
	
	void Update(float fTime, bool bUpdateInterpolators);

	void StartTransition(float fDuration)
	{
		Deactivate(0.0f, true);
		Activate(0, true, 1.0f, 0.0f, nullptr, true);
		Deactivate(fDuration, true);
	}

	void RemoveSingleInterps() const;
	bool StoreTargets(NiAVObject* pkRoot);

	float GetLastTime() const
	{
		return m_fLastTime;
	}

	float FindCorrespondingMorphFrame(NiControllerSequence* pkTargetSequence, float fTime) const;

	void SetInterpsWeightAndTime(float fWeight, float fEaseSpinner, float fTime);

	unsigned int AddInterpolator(NiInterpolator* pkInterpolator, const IDTag& idTag, unsigned char ucPriority)
	{
		return ThisStdCall(0xA32BC0, this, pkInterpolator, &idTag, ucPriority);
	}

};
ASSERT_SIZE(NiControllerSequence, 0x74);

using NiControllerSequencePtr = NiPointer<NiControllerSequence>;

// 06C
class BSAnimGroupSequence : public NiControllerSequence
{
public:
	BSAnimGroupSequence();
	~BSAnimGroupSequence();

	NiPointer<TESAnimGroup> animGroup;	//068

	BSAnimGroupSequence* Get3rdPersonCounterpart() const;
	float GetEaseInTime() const;
	float GetEaseOutTime() const;
};
STATIC_ASSERT(sizeof(BSAnimGroupSequence) == 0x78);

class NiInterpController : public NiTimeController
{
public:
	virtual UInt16 GetInterpolatorCount();
	virtual void *Unk_2E();
	virtual void *Unk_2F();
	virtual UInt16 GetInterpolatorIndexByIDTag(const NiControllerSequence::IDTag*) const;
	virtual NiInterpolator* GetInterpolator(UInt16 index) const;
	virtual void *SetInterpolator(NiInterpolator *, unsigned int);
	virtual void *Unk_33();
	virtual void *Unk_34();
	virtual NiInterpolator *CreatePoseInterpolator(unsigned short usIndex);
	virtual void *Unk_36();
	virtual void *Unk_37();
	virtual void *Unk_38(float, float);
	virtual void *Unk_39();

	NiAVObject* GetTargetNode(const NiControllerSequence::IDTag& idTag) const;
};

class NiBlendInterpolator : public NiInterpolator
{
public:
	virtual UInt8 AddInterpInfo(NiInterpolator *pkInterpolator, float fWeight, char cPriority, float fEaseSpinner = 1.0f);
	virtual NiInterpolator* RemoveInterpInfo(unsigned char ucIndex);
	virtual void *Unk_39();
	virtual void *Unk_3A();

	static constexpr unsigned char INVALID_INDEX = 0xFF;

	void ComputeNormalizedWeightsAdditive();
	void CalculatePrioritiesAdditive();

	enum
	{
		MANAGER_CONTROLLED_MASK = 0x0001,
		ONLY_USE_HIGHEST_WEIGHT_MASK = 0x0002,
		COMPUTE_NORMALIZED_WEIGHTS_MASK = 0x0004,
		HAS_ADDITIVE_TRANSFORMS_MASK = 0x0008 // custom kNVSE
	};
	
	struct InterpArrayItem
	{
		NiPointer<NiInterpolator> m_spInterpolator;
		float m_fWeight;
		float m_fNormalizedWeight;
		char m_cPriority;
		float m_fEaseSpinner;
		float m_fUpdateTime;
	};
	unsigned char m_uFlags;
	unsigned char m_ucArraySize;
	unsigned char m_ucInterpCount;
	unsigned char m_ucSingleIdx;
	char m_cHighPriority;
	char m_cNextHighPriority;
	InterpArrayItem* m_pkInterpArray;
	NiInterpolator* m_pkSingleInterpolator;
	float m_fWeightThreshold;
	float m_fSingleTime;
	float m_fHighSumOfWeights;
	float m_fNextHighSumOfWeights;
	float m_fHighEaseSpinner;

	InterpArrayItem* GetItemByInterpolator(const NiInterpolator* interpolator) const
	{
		for (auto& item : GetItems())
		{
			if (item.m_spInterpolator == interpolator)
				return &item;
		}
		return nullptr;
	}

	void ComputeNormalizedWeightsFor2Additive(InterpArrayItem* pkItem1, InterpArrayItem* pkItem2) const;
	
	std::span<InterpArrayItem> GetItems() const
	{
		return std::span(m_pkInterpArray, m_ucArraySize);
	}

	bool GetComputeNormalizedWeights() const
	{
		return GetBit(COMPUTE_NORMALIZED_WEIGHTS_MASK);
	}

	void SetComputeNormalizedWeights(bool bComputeNormalizedWeights)
	{
		SetBit(bComputeNormalizedWeights, COMPUTE_NORMALIZED_WEIGHTS_MASK);
	}

	bool GetOnlyUseHighestWeight() const
	{
		return GetBit(ONLY_USE_HIGHEST_WEIGHT_MASK);
	}

	bool GetManagerControlled() const
	{
		return GetBit(MANAGER_CONTROLLED_MASK);
	}

	bool GetHasAdditiveTransforms() const
	{
		return GetBit(HAS_ADDITIVE_TRANSFORMS_MASK);
	}

	void SetHasAdditiveTransforms(bool bAdditive)
	{
		SetBit(bAdditive, HAS_ADDITIVE_TRANSFORMS_MASK);
	}

	void RecalculateHighPriorities()
	{
		m_cNextHighPriority = INVALID_INDEX;
		m_cHighPriority = INVALID_INDEX;
		for (auto& item : GetItems())
		{
			if (item.m_spInterpolator != nullptr)
			{
				if (item.m_cPriority > m_cNextHighPriority)
				{
					if (item.m_cPriority > m_cHighPriority)
					{
						m_cNextHighPriority = m_cHighPriority;
						m_cHighPriority = item.m_cPriority;
					}
					else if (item.m_cPriority < m_cHighPriority)
					{
						m_cNextHighPriority = item.m_cPriority;
					}
				}
			}
		}
	}

	bool GetUpdateTimeForItem(float& fTime, InterpArrayItem& kItem)
	{
		NiInterpolator* pkInterpolator = kItem.m_spInterpolator.data;
		if (pkInterpolator && (kItem.m_fNormalizedWeight != 0.0f))
		{
			if (GetManagerControlled())
			{
				fTime = kItem.m_fUpdateTime;
			}

			if (fTime == INVALID_TIME)
			{
				return false;
			}
			return true;
		}
		return false;
	}

	void ComputeNormalizedWeights();
	void ComputeNormalizedWeightsHighPriorityDominant();

	void ClearWeightSums()
	{
		m_fHighSumOfWeights = -NI_INFINITY;
		m_fNextHighSumOfWeights = -NI_INFINITY;
		m_fHighEaseSpinner = -NI_INFINITY;
	}

	void SetPriority(char cPriority, unsigned char ucIndex)
	{
#ifdef _DEBUG
		NIASSERT(ucIndex < m_ucArraySize);
#endif
		// Only set priority if it differs from the current priority.
		if (m_pkInterpArray[ucIndex].m_cPriority == cPriority)
		{
			return;
		}

		m_pkInterpArray[ucIndex].m_cPriority = cPriority;

		if (cPriority > m_cHighPriority)
		{
			m_cNextHighPriority = m_cHighPriority;
			m_cHighPriority = cPriority;
		}
		else
		{
			// Determine highest priority.
			m_cHighPriority = m_cNextHighPriority = SCHAR_MIN;
			for (unsigned char uc = 0; uc < m_ucArraySize; uc++)
			{
				InterpArrayItem& kTempItem = m_pkInterpArray[uc];
				if (kTempItem.m_spInterpolator != NULL)
				{
					if (kTempItem.m_cPriority > m_cNextHighPriority)
					{
						if (kTempItem.m_cPriority > m_cHighPriority)
						{
							m_cNextHighPriority = m_cHighPriority;
							m_cHighPriority = kTempItem.m_cPriority;
						}
						else if (kTempItem.m_cPriority < m_cHighPriority)
						{
							m_cNextHighPriority = kTempItem.m_cPriority;
						}
					}
				}
			}
		}

		ClearWeightSums();
		SetComputeNormalizedWeights(true);
	}

	char GetFirstValidIndex() const
	{
		for (char uc = 0; uc < m_ucArraySize; uc++)
		{
			if (m_pkInterpArray[uc].m_spInterpolator != nullptr)
				return uc;
		}
		return INVALID_INDEX;
	}

	void SetWeight(float fWeight, unsigned char ucIndex)
	{
		NIASSERT(ucIndex < m_ucArraySize);
		NIASSERT(fWeight >= 0.0f);

		if (m_ucInterpCount == 1 && ucIndex == m_ucSingleIdx)
		{
			// Do not set the weight for a single interpolator.
			return;
		}

		if (m_pkInterpArray[ucIndex].m_fWeight == fWeight)
		{
			return;
		}

		m_pkInterpArray[ucIndex].m_fWeight = fWeight;
		ClearWeightSums();
		SetComputeNormalizedWeights(true);
	}

	void SetEaseSpinner(float fEaseSpinner, unsigned char ucIndex)
	{
		NIASSERT(ucIndex < m_ucArraySize);
		NIASSERT(fEaseSpinner >= 0.0f && fEaseSpinner <= 1.0f);


		if (m_ucInterpCount == 1 && ucIndex == m_ucSingleIdx)
		{
			// Do not set the ease spinner for a single interpolator.
			return;
		}

		if (m_pkInterpArray[ucIndex].m_fEaseSpinner == fEaseSpinner)
		{
			return;
		}

		m_pkInterpArray[ucIndex].m_fEaseSpinner = fEaseSpinner;
		ClearWeightSums();
		SetComputeNormalizedWeights(true);
	}

	void SetTime(float fTime, unsigned char ucIndex)
	{
		NIASSERT(ucIndex < m_ucArraySize);
		
		if (m_ucInterpCount == 1 && ucIndex == m_ucSingleIdx)
		{
			// Set the cached time for a single interpolator.
			m_fSingleTime = fTime;
			return;
		}

		m_pkInterpArray[ucIndex].m_fUpdateTime = fTime;
	}

};
static_assert(sizeof(NiBlendInterpolator) == 0x30);

class NiBlendAccumTransformInterpolator : public NiBlendInterpolator
{
public:
	struct AccumArrayItem
	{
		float m_fLastTime;
		NiQuatTransform m_kLastValue;
		NiQuatTransform m_kDeltaValue;
		NiMatrix33 m_kRefFrame;
	};
	
	NiQuatTransform m_kAccumulatedTransformValue;
	AccumArrayItem *m_pkAccumArray;
	bool m_bReset;

	bool BlendValues(float fTime, NiObjectNET* pkInterpTarget, NiQuatTransform& kValue);
};

class NiBlendTransformInterpolator : public NiBlendInterpolator
{
	bool GetSingleUpdateTime(float& fTime)
	{
		NIASSERT(m_ucSingleIdx != INVALID_INDEX && 
			m_pkSingleInterpolator != NULL);
    
		if (GetManagerControlled())
		{
			fTime = m_fSingleTime;
		}

		if (fTime == INVALID_TIME)
		{
			// The time for this interpolator has not been set. Do
			// not update the interpolator.
			return false;
		}

		return true;
	}


public:
	static NiBlendTransformInterpolator* Create()
	{
		return CdeclCall<NiBlendTransformInterpolator*>(0xA409D0);
	}
	
	void ApplyAdditiveTransforms(float fTime, NiObjectNET* pkInterpTarget, NiQuatTransform& kValue) const;

	bool BlendValues(float fTime, NiObjectNET* pkInterpTarget,
					 NiQuatTransform& kValue);

	bool BlendValuesFixFloatingPointError(float fTime, NiObjectNET* pkInterpTarget,
		NiQuatTransform& kValue);

	bool _Update(float fTime, NiObjectNET* pkInterpTarget, NiQuatTransform& kValue);

	bool StoreSingleValue(float fTime, NiObjectNET* pkInterpTarget, NiQuatTransform& kValue)
	{
		if (!GetSingleUpdateTime(fTime))
		{
			kValue.MakeInvalid();
			return false;
		}

		NIASSERT(m_pkSingleInterpolator != NULL);
		if (m_pkSingleInterpolator->Update(fTime, pkInterpTarget, kValue))
		{
			return true;
		}
		else
		{
			kValue.MakeInvalid();
			return false;
		}
	}
};

/* 44137 */
class NiMultiTargetTransformController : public NiInterpController
{
public:
	NiBlendTransformInterpolator* m_pkBlendInterps;
	NiAVObject** m_ppkTargets;
	UInt16 m_usNumInterps;

	void _Update(float fTime, bool bSelective);

	std::span<NiAVObject*> GetTargets() const
	{
		return std::span(m_ppkTargets, m_usNumInterps);
	}
};

const auto s = sizeof(BSAnimGroupSequence);

class NiNode;

enum AnimGroupID : UInt8
{
	kAnimGroup_Idle					= 0x0,
	kAnimGroup_DynamicIdle,
	kAnimGroup_SpecialIdle,
	kAnimGroup_Forward,
	kAnimGroup_Backward,
	kAnimGroup_Left,
	kAnimGroup_Right,
	kAnimGroup_FastForward,
	kAnimGroup_FastBackward,
	kAnimGroup_FastLeft,
	kAnimGroup_FastRight,
	kAnimGroup_DodgeForward,
	kAnimGroup_DodgeBack,
	kAnimGroup_DodgeLeft,
	kAnimGroup_DodgeRight,
	kAnimGroup_TurnLeft,
	kAnimGroup_TurnRight,
	kAnimGroup_Aim,
	kAnimGroup_AimUp,
	kAnimGroup_AimDown,
	kAnimGroup_AimIS,
	kAnimGroup_AimISUp,
	kAnimGroup_AimISDown,
	kAnimGroup_Holster,
	kAnimGroup_Equip,
	kAnimGroup_Unequip,
	kAnimGroup_AttackLeft,
	kAnimGroup_AttackLeftUp,
	kAnimGroup_AttackLeftDown,
	kAnimGroup_AttackLeftIS,
	kAnimGroup_AttackLeftISUp,
	kAnimGroup_AttackLeftISDown,
	kAnimGroup_AttackRight,
	kAnimGroup_AttackRightUp,
	kAnimGroup_AttackRightDown,
	kAnimGroup_AttackRightIS,
	kAnimGroup_AttackRightISUp,
	kAnimGroup_AttackRightISDown,
	kAnimGroup_Attack3,
	kAnimGroup_Attack3Up,
	kAnimGroup_Attack3Down,
	kAnimGroup_Attack3IS,
	kAnimGroup_Attack3ISUp,
	kAnimGroup_Attack3ISDown,
	kAnimGroup_Attack4,
	kAnimGroup_Attack4Up,
	kAnimGroup_Attack4Down,
	kAnimGroup_Attack4IS,
	kAnimGroup_Attack4ISUp,
	kAnimGroup_Attack4ISDown,
	kAnimGroup_Attack5,
	kAnimGroup_Attack5Up,
	kAnimGroup_Attack5Down,
	kAnimGroup_Attack5IS,
	kAnimGroup_Attack5ISUp,
	kAnimGroup_Attack5ISDown,
	kAnimGroup_Attack6,
	kAnimGroup_Attack6Up,
	kAnimGroup_Attack6Down,
	kAnimGroup_Attack6IS,
	kAnimGroup_Attack6ISUp,
	kAnimGroup_Attack6ISDown,
	kAnimGroup_Attack7,
	kAnimGroup_Attack7Up,
	kAnimGroup_Attack7Down,
	kAnimGroup_Attack7IS,
	kAnimGroup_Attack7ISUp,
	kAnimGroup_Attack7ISDown,
	kAnimGroup_Attack8,
	kAnimGroup_Attack8Up,
	kAnimGroup_Attack8Down,
	kAnimGroup_Attack8IS,
	kAnimGroup_Attack8ISUp,
	kAnimGroup_Attack8ISDown,
	kAnimGroup_AttackLoop,
	kAnimGroup_AttackLoopUp,
	kAnimGroup_AttackLoopDown,
	kAnimGroup_AttackLoopIS,
	kAnimGroup_AttackLoopISUp,
	kAnimGroup_AttackLoopISDown,
	kAnimGroup_AttackSpin,
	kAnimGroup_AttackSpinUp,
	kAnimGroup_AttackSpinDown,
	kAnimGroup_AttackSpinIS,
	kAnimGroup_AttackSpinISUp,
	kAnimGroup_AttackSpinISDown,
	kAnimGroup_AttackSpin2,
	kAnimGroup_AttackSpin2Up,
	kAnimGroup_AttackSpin2Down,
	kAnimGroup_AttackSpin2IS,
	kAnimGroup_AttackSpin2ISUp,
	kAnimGroup_AttackSpin2ISDown,
	kAnimGroup_AttackPower,
	kAnimGroup_AttackForwardPower,
	kAnimGroup_AttackBackPower,
	kAnimGroup_AttackLeftPower,
	kAnimGroup_AttackRightPower,
	kAnimGroup_AttackCustom1Power,
	kAnimGroup_AttackCustom2Power,
	kAnimGroup_AttackCustom3Power,
	kAnimGroup_AttackCustom4Power,
	kAnimGroup_AttackCustom5Power,
	kAnimGroup_PlaceMine,
	kAnimGroup_PlaceMineUp,
	kAnimGroup_PlaceMineDown,
	kAnimGroup_PlaceMineIS,
	kAnimGroup_PlaceMineISUp,
	kAnimGroup_PlaceMineISDown,
	kAnimGroup_PlaceMine2,
	kAnimGroup_PlaceMine2Up,
	kAnimGroup_PlaceMine2Down,
	kAnimGroup_PlaceMine2IS,
	kAnimGroup_PlaceMine2ISUp,
	kAnimGroup_PlaceMine2ISDown,
	kAnimGroup_AttackThrow,
	kAnimGroup_AttackThrowUp,
	kAnimGroup_AttackThrowDown,
	kAnimGroup_AttackThrowIS,
	kAnimGroup_AttackThrowISUp,
	kAnimGroup_AttackThrowISDown,
	kAnimGroup_AttackThrow2,
	kAnimGroup_AttackThrow2Up,
	kAnimGroup_AttackThrow2Down,
	kAnimGroup_AttackThrow2IS,
	kAnimGroup_AttackThrow2ISUp,
	kAnimGroup_AttackThrow2ISDown,
	kAnimGroup_AttackThrow3,
	kAnimGroup_AttackThrow3Up,
	kAnimGroup_AttackThrow3Down,
	kAnimGroup_AttackThrow3IS,
	kAnimGroup_AttackThrow3ISUp,
	kAnimGroup_AttackThrow3ISDown,
	kAnimGroup_AttackThrow4,
	kAnimGroup_AttackThrow4Up,
	kAnimGroup_AttackThrow4Down,
	kAnimGroup_AttackThrow4IS,
	kAnimGroup_AttackThrow4ISUp,
	kAnimGroup_AttackThrow4ISDown,
	kAnimGroup_AttackThrow5,
	kAnimGroup_AttackThrow5Up,
	kAnimGroup_AttackThrow5Down,
	kAnimGroup_AttackThrow5IS,
	kAnimGroup_AttackThrow5ISUp,
	kAnimGroup_AttackThrow5ISDown,
	kAnimGroup_Attack9,
	kAnimGroup_Attack9Up,
	kAnimGroup_Attack9Down,
	kAnimGroup_Attack9IS,
	kAnimGroup_Attack9ISUp,
	kAnimGroup_Attack9ISDown,
	kAnimGroup_AttackThrow6,
	kAnimGroup_AttackThrow6Up,
	kAnimGroup_AttackThrow6Down,
	kAnimGroup_AttackThrow6IS,
	kAnimGroup_AttackThrow6ISUp,
	kAnimGroup_AttackThrow6ISDown,
	kAnimGroup_AttackThrow7,
	kAnimGroup_AttackThrow7Up,
	kAnimGroup_AttackThrow7Down,
	kAnimGroup_AttackThrow7IS,
	kAnimGroup_AttackThrow7ISUp,
	kAnimGroup_AttackThrow7ISDown,
	kAnimGroup_AttackThrow8,
	kAnimGroup_AttackThrow8Up,
	kAnimGroup_AttackThrow8Down,
	kAnimGroup_AttackThrow8IS,
	kAnimGroup_AttackThrow8ISUp,
	kAnimGroup_AttackThrow8ISDown,
	kAnimGroup_Counter,
	kAnimGroup_stomp,
	kAnimGroup_BlockIdle,
	kAnimGroup_BlockHit,
	kAnimGroup_Recoil,
	kAnimGroup_ReloadWStart,
	kAnimGroup_ReloadXStart,
	kAnimGroup_ReloadYStart,
	kAnimGroup_ReloadZStart,
	kAnimGroup_ReloadA,
	kAnimGroup_ReloadB,
	kAnimGroup_ReloadC,
	kAnimGroup_ReloadD,
	kAnimGroup_ReloadE,
	kAnimGroup_ReloadF,
	kAnimGroup_ReloadG,
	kAnimGroup_ReloadH,
	kAnimGroup_ReloadI,
	kAnimGroup_ReloadJ,
	kAnimGroup_ReloadK,
	kAnimGroup_ReloadL,
	kAnimGroup_ReloadM,
	kAnimGroup_ReloadN,
	kAnimGroup_ReloadO,
	kAnimGroup_ReloadP,
	kAnimGroup_ReloadQ,
	kAnimGroup_ReloadR,
	kAnimGroup_ReloadS,
	kAnimGroup_ReloadW,
	kAnimGroup_ReloadX,
	kAnimGroup_ReloadY,
	kAnimGroup_ReloadZ,
	kAnimGroup_JamA,
	kAnimGroup_JamB,
	kAnimGroup_JamC,
	kAnimGroup_JamD,
	kAnimGroup_JamE,
	kAnimGroup_JamF,
	kAnimGroup_JamG,
	kAnimGroup_JamH,
	kAnimGroup_JamI,
	kAnimGroup_JamJ,
	kAnimGroup_JamK,
	kAnimGroup_JamL,
	kAnimGroup_JamM,
	kAnimGroup_JamN,
	kAnimGroup_JamO,
	kAnimGroup_JamP,
	kAnimGroup_JamQ,
	kAnimGroup_JamR,
	kAnimGroup_JamS,
	kAnimGroup_JamW,
	kAnimGroup_JamX,
	kAnimGroup_JamY,
	kAnimGroup_JamZ,
	kAnimGroup_Stagger,
	kAnimGroup_Death,
	kAnimGroup_Talking,
	kAnimGroup_PipBoy,
	kAnimGroup_JumpStart,
	kAnimGroup_JumpLoop,
	kAnimGroup_JumpLand,
	kAnimGroup_HandGrip1,
	kAnimGroup_HandGrip2,
	kAnimGroup_HandGrip3,
	kAnimGroup_HandGrip4,
	kAnimGroup_HandGrip5,
	kAnimGroup_HandGrip6,
	kAnimGroup_JumpLoopForward,
	kAnimGroup_JumpLoopBackward,
	kAnimGroup_JumpLoopLeft,
	kAnimGroup_JumpLoopRight,
	kAnimGroup_PipBoyChild,
	kAnimGroup_JumpLandForward,
	kAnimGroup_JumpLandBackward,
	kAnimGroup_JumpLandLeft,
	kAnimGroup_JumpLandRight,

	kAnimGroup_Max,						// = 0x0FFF,	// Temporary until known

	kAnimGroup_Invalid = 0xFF
};

class NiAVObjectPalette : public NiObject
{
};


class NiDefaultAVObjectPalette : public NiAVObjectPalette
{
public:
	NiTStringPointerMap<NiAVObject> m_kHash;
	NiAVObject* m_pkScene;
};


class NiDefaultAVObjectPalette;
// 7C
class NiControllerManager : public NiTimeController
{
public:
	virtual void	Unk_2D(void);

	NiTArray<NiControllerSequence*>	sequences;		// 34
	NiTSet<NiControllerSequence*> m_kActiveSequences;
	NiTStringPointerMap<NiControllerSequence> m_kSequenceMap;
	NiTArray<void*> *pListener;
	bool m_bCumulitive;
	NiTSet<NiControllerSequence*> m_kTempBlendSeqs;
	NiDefaultAVObjectPalette* m_spObjectPalette;

	bool BlendFromPose(NiControllerSequence* pkSequence, float fDestFrame,
		float fDuration, int iPriority = 0,
		NiControllerSequence* pkSequenceToSynchronize = NULL);
	bool CrossFade(NiControllerSequence* pkSourceSequence,
		NiControllerSequence* pkDestSequence, float fDuration,
		int iPriority = 0, bool bStartOver = false, float fWeight = 1.0f,
		NiControllerSequence* pkTimeSyncSeq = NULL);
	NiControllerSequence* CreateTempBlendSequence(
		NiControllerSequence* pkSequence,
		NiControllerSequence* pkSequenceToSynchronize);
	bool Morph(NiControllerSequence* pkSourceSequence,
	           NiControllerSequence* pkDestSequence, float fDuration, int iPriority,
	           float fSourceWeight, float fDestWeight);
	bool ActivateSequence(NiControllerSequence* pkSequence, int iPriority, bool bStartOver,
		float fWeight, float fEaseInTime, NiControllerSequence* pkTimeSyncSeq);
	bool DeactivateSequence(NiControllerSequence* pkSequence, float fEaseOutTime);
	bool AddSequence(NiControllerSequence* pkSequence, const NiFixedString& kName, bool bStoreTargets)
	{
		return ThisStdCall(0xA2F0C0, this, pkSequence, &kName, bStoreTargets);
	}

	template <typename T>
	NiControllerSequence* FindSequence(T&& predicate) const
	{
		const auto activeSequences = m_kActiveSequences.ToSpan();
		auto it = std::ranges::find_if(activeSequences, predicate);
		return it != activeSequences.end() ? *it : nullptr;
	}

	NiControllerSequence* GetTempBlendSequence() const
	{
		return FindSequence([](const NiControllerSequence* seq)
		{
			return std::string_view(seq->m_kName.CStr()).starts_with("__");
		});
	}

	NiControllerSequence* GetInterpolatorOwner(const NiInterpolator* interpolator)
	{
		for (auto* sequence : m_kActiveSequences)
		{
			for (auto& controlledBlock : sequence->GetControlledBlocks())
			{
				if (controlledBlock.m_spInterpolator == interpolator)
					return sequence;
			}
		}
		return nullptr;
	}
	
};
static_assert(sizeof(NiControllerManager) == 0x7C);

enum eAnimSequence
{
	kSequence_None = -0x1,
	kSequence_Idle = 0x0,
	kSequence_Movement = 0x1,
	kSequence_LeftArm = 0x2,
	kSequence_LeftHand = 0x3,
	kSequence_Weapon = 0x4,
	kSequence_WeaponUp = 0x5,
	kSequence_WeaponDown = 0x6,
	kSequence_SpecialIdle = 0x7,
	kSequence_Death = 0x14,
};

struct AnimGroupInfo
{
	const char* name;
	bool supportsVariants;
	UInt8 pad[3];
	UInt32 sequenceType;
	UInt32 keyType;
	UInt32 unk10;
	UInt32 unk14[4];
};

template <typename T>
class NiFixedArray
{
public:
	unsigned int m_uiNumItems;
	T* m_pData;

	NiFixedArray()
		: m_uiNumItems(0),
		  m_pData(nullptr)
	{
	}

	NiFixedArray(const NiFixedArray& other)
			: m_uiNumItems(other.m_uiNumItems),
			  m_pData(GameHeapAllocArray<T>(other.m_uiNumItems))
	{
		std::ranges::copy(other.GetItems(), m_pData);
	}

	NiFixedArray(NiFixedArray&& other) noexcept
		: m_uiNumItems(other.m_uiNumItems),
		  m_pData(other.m_pData)
	{
		other.m_uiNumItems = 0;
		other.m_pData = nullptr;
	}

	template <std::ranges::sized_range R>
	NiFixedArray(R&& range)
	: m_uiNumItems(std::ranges::size(range)),
	  m_pData(GameHeapAllocArray<T>(m_uiNumItems))
	{
		std::ranges::copy(range, m_pData);
	}

	template <std::ranges::input_range R>
	requires (!std::ranges::sized_range<R>)
	NiFixedArray(R&& view)
	{
		m_uiNumItems = std::ranges::distance(view);
		m_pData = GameHeapAllocArray<T>(m_uiNumItems);

		std::ranges::copy(view, m_pData);
	}

	NiFixedArray& operator=(const NiFixedArray& other)
	{
		if (this == &other)
			return *this;

		DeleteAll();

		m_uiNumItems = other.m_uiNumItems;
		m_pData = GameHeapAllocArray<T>(other.m_uiNumItems);
		std::copy(other.m_pData, other.m_pData + other.m_uiNumItems, m_pData);
		return *this;
	}

	NiFixedArray& operator=(NiFixedArray&& other) noexcept
	{
		if (this == &other)
			return *this;

		DeleteAll();

		m_uiNumItems = other.m_uiNumItems;
		m_pData = other.m_pData;
		other.m_uiNumItems = 0;
		other.m_pData = nullptr;
		return *this;
	}

	void DeleteAll()
	{
		for (auto& item : GetItems())
			std::destroy_at(std::addressof(item));
		GameHeapFreeArray(m_pData);
		m_uiNumItems = 0;
	}

	~NiFixedArray()
	{
		DeleteAll();
	}

	std::span<T> GetItems() const
	{
		return { m_pData, m_uiNumItems };
	}

	std::vector<T> ToVector() const
	{
		return std::vector(m_pData, m_pData + m_uiNumItems);
	}

	operator std::span<T>() const
	{
		return GetItems();
	}

	// operator []
	T& operator[](size_t index)
	{
		return m_pData[index];
	}
};
ASSERT_SIZE(NiFixedArray<float>, 0x8);

// 02C+
class TESAnimGroup : public NiRefObject
{
public:
	// derived from NiRefObject
	TESAnimGroup();
	~TESAnimGroup();

	virtual void Destructor(bool arg0);

	bool IsAttack()
	{
		return ThisStdCall(0x4937E0, this);
	}

	bool IsAttackIS();
	bool IsAttackNonIS();

	bool IsLoopingReloadStart() const;
	bool IsLoopingReload() const;

	bool IsAim()
	{
		const auto idMinor = groupID & 0xFF;
		return idMinor >= kAnimGroup_Aim && idMinor <= kAnimGroup_AimISDown;
	}

	bool IsReload()
	{
		const auto idMinor = groupID & 0xFF;
		return idMinor >= kAnimGroup_ReloadWStart && idMinor <= kAnimGroup_ReloadZ;
	}
	
	// 24
	
	AnimGroupInfo* GetGroupInfo() const;

	eAnimSequence GetSequenceType() const
	{
		return static_cast<eAnimSequence>(GetGroupInfo()->sequenceType);
	}

	AnimGroupID GetBaseGroupID() const;

	bool IsTurning() const
	{
		auto animGroupId = GetBaseGroupID();
		return animGroupId == kAnimGroup_TurnLeft || animGroupId == kAnimGroup_TurnRight;
	}

	bool IsJumpLand() const
	{
		switch (GetBaseGroupID())
		{
		case kAnimGroup_JumpLand:
		case kAnimGroup_JumpLandForward:
		case kAnimGroup_JumpLandBackward:
		case kAnimGroup_JumpLandLeft:
		case kAnimGroup_JumpLandRight:
			return true;
		default:
			return false;
		}
	}

	bool IsBaseMovement() const
	{
		return GetSequenceType() == kSequence_Movement && !IsTurning();
	}

	struct AnimGroupSound
	{
		float playTime;
		UInt8 soundID;
		UInt8 gap05[3];
		float speed;
		TESSound* sound;
	};


	UInt8 byte08[8];
	UInt16 groupID;
	UInt8 unk12;
	SimpleFixedArray<float>	keyTimes;
	NiPoint3 moveVector;
	UInt8 leftOrRight_whichFootToSwitch;
	UInt8 blend;
	UInt8 blendIn;
	UInt8 blendOut;
	UInt8 decal;
	UInt8 gap2D[3];
	NiFixedString parentRootNode;
	SimpleFixedArray<AnimGroupSound> sounds;

	static const char* StringForAnimGroupCode(UInt32 groupCode);
	static UInt32 AnimGroupForString(const char* groupName);

	UInt8 GetBlendIn() const
	{
		return ThisStdCall(0x4954E0, this);
	}

	UInt8 GetBlendOut() const
	{
		return ThisStdCall(0x495520, this);
	}

	UInt16 GetMoveType() const;

	static TESAnimGroup* Init(BSAnimGroupSequence* sequence, const char* path)
	{
		return CdeclCall<TESAnimGroup*>(0x5F3A20, sequence, path);
	}
};
STATIC_ASSERT(sizeof(TESAnimGroup) == 0x3C);

void DumpAnimGroups(void);

class NiBinaryStream
{
public:
	NiBinaryStream();

	virtual ~NiBinaryStream();		// 00
	virtual void	Unk_01(void);						// 04
	virtual void	SeekCur(SInt32 delta);				// 08
	virtual void	GetBufferSize(void);				// 0C
	virtual void	InitReadWriteProcs(bool useAlt);	// 10

//	void	** m_vtbl;		// 000
	UInt32	m_uiAbsoluteCurrentPos;		// 004
	void	* m_pfnRead;	// 008 - function pointer
	void	* m_pfnWrite;	// 00C - function pointer
};

class NiFile: public NiBinaryStream
{
public:
	enum OpenMode {
		READ_ONLY	= 0,
		WRITE_ONLY	= 1,
		APPEND_ONLY = 2,
	};
	
	NiFile();
	~NiFile();

	virtual void		Seek(SInt32 aiOffset, SInt32 aiWhence);
	virtual const char*	GetFilename() const;
	virtual UInt32		GetFileSize();

	UInt32		m_uiBufferAllocSize;
	UInt32		m_uiBufferReadSize;
	UInt32		m_uiPos;
	UInt32		m_uiAbsolutePos;
	char*		m_pBuffer;
	FILE*		m_pFile;
	OpenMode	m_eMode;
	bool		m_bGood;
};
ASSERT_SIZE(NiFile, 0x30);

//// derived from NiFile, which derives from NiBinaryStream
//// 154
//class BSFile
//{
//public:
//	BSFile();
//	~BSFile();
//
//	virtual void	Destructor(bool freeMemory);				// 00
//	virtual void	Unk_01(void);								// 04
//	virtual void	Unk_02(void);								// 08
//	virtual void	Unk_03(void);								// 0C
//	virtual void	Unk_04(void);								// 10
//	virtual void	DumpAttributes(NiTArray <char *> * dst);	// 14
//	virtual UInt32	GetSize(void);								// 18
//	virtual void	Unk_07(void);								// 1C
//	virtual void	Unk_08(void);								// 20
//	virtual void	Unk_09(void);								// 24
//	virtual void	Unk_0A(void);								// 28
//	virtual void	Unk_0B(void);								// 2C
//	virtual void	Unk_0C(void);								// 30
//	virtual void	Unk_Read(void);								// 34
//	virtual void	Unk_Write(void);							// 38
//
////	void	** m_vtbl;		// 000
//	void	* m_readProc;	// 004 - function pointer
//	void	* m_writeProc;	// 008 - function pointer
//	UInt32	m_bufSize;		// 00C
//	UInt32	m_unk010;		// 010 - init'd to m_bufSize
//	UInt32	m_unk014;		// 014
//	void	* m_buf;		// 018
//	FILE	* m_file;		// 01C
//	UInt32	m_writeAccess;	// 020
//	UInt8	m_good;			// 024
//	UInt8	m_pad025[3];	// 025
//	UInt8	m_unk028;		// 028
//	UInt8	m_pad029[3];	// 029
//	UInt32	m_unk02C;		// 02C
//	UInt32	m_pos;			// 030
//	UInt32	m_unk034;		// 034
//	UInt32	m_unk038;		// 038
//	char	m_path[0x104];	// 03C
//	UInt32	m_unk140;		// 140
//	UInt32	m_unk144;		// 144
//	UInt32	m_pos2;			// 148 - used if m_pos is 0xFFFFFFFF
//	UInt32	m_unk14C;		// 14C
//	UInt32	m_fileSize;		// 150
//};

/**** misc non-NiObjects ****/

// 30
class NiPropertyState : public NiRefObject
{
public:
	NiPropertyState();
	~NiPropertyState();

	UInt32	unk008[(0x30 - 0x08) >> 2];	// 008
};

// 20
class NiDynamicEffectState : public NiRefObject
{
public:
	NiDynamicEffectState();
	~NiDynamicEffectState();

	UInt8	unk008;		// 008
	UInt8	pad009[3];	// 009
	UInt32	unk00C;		// 00C
	UInt32	unk010;		// 010
	UInt32	unk014;		// 014
	UInt32	unk018;		// 018
	UInt32	unk01C;		// 01C
};

// name is a guess
class NiCulledGeoList
{
public:
	NiCulledGeoList();
	~NiCulledGeoList();

	NiGeometry	** m_geo;		// 00
	UInt32		m_numItems;		// 04
	UInt32		m_bufLen;		// 08
	UInt32		m_bufGrowSize;	// 0C
};

// 90
class NiCullingProcess
{
public:
	NiCullingProcess();
	~NiCullingProcess();

	virtual void	Destructor(bool freeMemory);
	virtual void	Unk_01(void * arg);
	virtual void	Cull(NiCamera * camera, NiAVObject * scene, NiCulledGeoList * culledGeo);
	virtual void	AddGeo(NiGeometry * arg);

//	void			** m_vtbl;		// 00
	UInt8			m_useAddGeoFn;	// 04 - call AddGeo when true, else just add to the list
	UInt8			pad05[3];		// 05
	NiCulledGeoList	* m_culledGeo;	// 08
};

/**** BSTempEffects ****/

// 18
class BSTempEffect : public NiObject
{
public:
	BSTempEffect();
	~BSTempEffect();

	float			duration;		// 08
	TESObjectCELL*	cell;			// 0C
	float			unk10;			// 10
	UInt8			unk14;			// 14
	UInt8			pad15[3];
};

// 28
class MagicHitEffect : public BSTempEffect
{
public:
	MagicHitEffect();
	~MagicHitEffect();

	ActiveEffect	* activeEffect;	// 18	
	TESObjectREFR	* target;		// 1C
	float			unk20;			// 20	Init'd from ActiveEffect.timeElapsed
	UInt8			unk24;			// 24	from ActiveEffect.EffectFlag
	UInt8			pad25[3];
};

// 6C
class MagicShaderHitEffect : public MagicHitEffect
{
public:
	MagicShaderHitEffect();
	~MagicShaderHitEffect();

	UInt8					unk28;						// 28	Init'd to byte, OK for first offset.
	UInt8					pad29[3];
	UInt32					unk2C;						// 2C	Init'd to DWord
	TESEffectShader			* effectShader;				// 30	Init'd to *effectShader
	float					unk34;						// 34	Init'd to float
	BSSimpleArray<NiPointer<ParticleShaderProperty>>	unk38;	// 38	Init'd to BSSimpleArray<NiPointer<ParticleShaderProperty>>
	// the remainder is not validated..
	void					* textureEffectData;		// 48 seen TextureEffectData< BSSahderLightingProperty >, init'd to RefNiObject
};	// Alloc'd to 6C, 68 is RefNiObject, 60 is Init'd to 1.0, 64 also
	// 4C is byte, Init'd to 0 for non player, otherwize = Player.1stPersonSkeleton.Flags0030.Bit0 is null


class NiTextKey : public NiMemObject
{
public:
	float m_fTime;
	NiFixedString m_kText;

	NiTextKey(float mFTime, const NiFixedString& mKText)
		: m_fTime(mFTime),
		  m_kText(mKText)
	{
	}

	void SetText(const char* text)
	{
		m_kText = text;
	}

	void SetTime(float time)
	{
		m_fTime = time;
	}
};


class NiExtraData : public NiObject
{
public:
	NiFixedString m_kName;

	NIRTTI_ADDRESS(0x11F4A80);
};

class NiTextKeyExtraData : public NiExtraData
{
public:
	NiFixedArray<NiTextKey> m_kKeyArray;

	NiTextKey* FindFirstByName(NiFixedString name) const
	{
		const auto keys = GetKeys();
		auto it = std::ranges::find_if(keys, [name](const NiTextKey& key) {
			return key.m_kText == name;
		});
		return it != keys.end() ? &*it : nullptr;
	}

	NiTextKey* FindFirstThatStartsWith(const char* name) const;

	std::span<NiTextKey> GetKeys() const
	{
		return m_kKeyArray.GetItems();
	}

	std::vector<NiTextKey> ToVector() const
	{
		return m_kKeyArray.ToVector();
	}

	template <std::ranges::input_range R>
	void SetKeys(const R& keys)
	{
		m_kKeyArray = keys;
	}

	NiTextKey* AddKey(NiFixedString name, float time)
	{
		auto vec = ToVector();
		vec.push_back(NiTextKey(time, name));
		m_kKeyArray = vec;
		return &m_kKeyArray.GetItems().back();
	}

	NiTextKey* SetOrAddKey(NiFixedString name, float time)
	{
		auto* key = FindFirstByName(name);
		if (key)
			key->SetTime(time);
		else
			AddKey(name, time);
		return key;
	}

	bool RemoveKey(size_t index)
	{
		if (index >= m_kKeyArray.GetItems().size())
			return false;
		m_kKeyArray = GetKeys() | std::views::drop(index) | std::ranges::to<std::vector>();
		return true;
	}

	operator std::span<NiTextKey>() const
	{
		return m_kKeyArray;
	}

};
static_assert(sizeof(NiTextKeyExtraData) == 0x14);

class NiBSplineData : public NiObject
{
public:
	float *m_pafControlPoints;
	short *m_pasCompactControlPoints;
	unsigned int m_uiControlPointCount;
	unsigned int m_uiCompactControlPointCount;
};

template <class REAL, int DEGREE>
class NiBSplineBasis : public NiMemObject {
public:
    int32_t             m_iQuantity;
    mutable REAL        m_afValue[DEGREE + 1];
    mutable REAL        m_fLastTime;
    mutable int32_t     m_iMin;
    mutable int32_t     m_iMax;
};

class NiBSplineBasisData : public NiObject
{
public:
	NiBSplineBasis<float,3> m_kBasisDegree3;
};

class NiBSplineInterpolator : public NiInterpolator
{
public:
	float m_fStartTime;
	float m_fEndTime;
	NiPointer<NiBSplineData> m_spData;
	NiPointer<NiBSplineBasisData> m_spBasisData;
};

class NiBSplineTransformInterpolator : public NiBSplineInterpolator
{
public:
	NiQuatTransform m_kTransformValue;
	unsigned int m_kTransCPHandle;
	unsigned int m_kRotCPHandle;
	unsigned int m_kScaleCPHandle;
};

class NiBSplineCompTransformInterpolator : public NiBSplineTransformInterpolator
{
public:
	float m_afCompScalars[6];
};