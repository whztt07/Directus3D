/*
Copyright(c) 2016-2018 Panos Karabelas

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
copies of the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

//= INCLUDES =============================
#include "Light.h"
#include "Transform.h"
#include "Camera.h"
#include "Renderable.h"
#include "../../World/Actor.h"
#include "../../IO/FileStream.h"
#include "../../Rendering/Renderer.h"
#include "../../RHI/RHI_RenderTexture.h"
//========================================

//= NAMESPACES ================
using namespace Directus::Math;
using namespace std;
//=============================

namespace Directus
{
	Light::Light(Context* context, Actor* actor, Transform* transform) : IComponent(context, actor, transform)
	{
		REGISTER_ATTRIBUTE_VALUE_VALUE(m_castShadows, bool);
		REGISTER_ATTRIBUTE_VALUE_VALUE(m_range, float);
		REGISTER_ATTRIBUTE_VALUE_VALUE(m_intensity, float);
		REGISTER_ATTRIBUTE_VALUE_VALUE(m_angle, float);
		REGISTER_ATTRIBUTE_VALUE_VALUE(m_color, Vector4);
		REGISTER_ATTRIBUTE_VALUE_VALUE(m_bias, float);
		REGISTER_ATTRIBUTE_GET_SET(GetLightType, SetLightType, LightType);

		m_lightType		= LightType_Point;
		m_castShadows	= true;
		m_range			= 1.0f;
		m_intensity		= 2.0f;
		m_angle			= 0.5f; // about 30 degrees
		m_color			= Vector4(1.0f, 0.76f, 0.57f, 1.0f);
		m_bias			= 0.001f;	
		m_isDirty		= true;

		// Shadow map splits (for directional light's cascades)
		m_shadowMapSplits.clear();
		m_shadowMapSplits.shrink_to_fit();
		m_shadowMapSplits.emplace_back(0.01f);
		m_shadowMapSplits.emplace_back(0.035f);
	}

	Light::~Light()
	{
		ShadowMap_Destroy();
	}

	void Light::OnInitialize()
	{
		ShadowMap_Create(true);
	}

	void Light::OnStart()
	{
		ShadowMap_Create(false);
	}

	void Light::OnTick()
	{
		if (m_lightType != LightType_Directional)
			return;

		// DIRTY CHECK
		if (m_lastPosLight != GetTransform()->GetPosition() || m_lastRotLight != GetTransform()->GetRotation())
		{
			m_lastPosLight = GetTransform()->GetPosition();
			m_lastRotLight = GetTransform()->GetRotation();
			
			// Used to prevent directional light
			// from casting shadows form underneath
			// the scene which can look weird
			ClampRotation();
			ComputeViewMatrix();

			m_isDirty = true;
		}

		Camera* camera = m_context->GetSubsystem<World>()->GetMainCamera().lock()->GetComponent<Camera>().lock().get();
		if (m_lastPosCamera != camera->GetTransform()->GetPosition())
		{
			m_lastPosCamera = camera->GetTransform()->GetPosition();

			// Update shadow map projection matrices
			m_shadowMapsProjectionMatrix.clear();
			for (unsigned int i = 0; i < m_shadowMapCount; i++)
			{
				m_shadowMapsProjectionMatrix.emplace_back(Matrix());
				ShadowMap_ComputeProjectionMatrix(i);
			}

			m_isDirty = true;
		}

		if (!m_isDirty)
			return;

		if (auto mainCamera = GetContext()->GetSubsystem<World>()->GetMainCamera().lock().get())
		{
			if (auto cameraComp = mainCamera->GetComponent<Camera>().lock().get())
			{
				for (unsigned int index = 0; index < (unsigned int)m_frustums.size(); index++)
				{
					m_frustums[index]->Construct(m_viewMatrix, ShadowMap_GetProjectionMatrix(index), cameraComp->GetFarPlane());
				}
			}
		}
	}

	void Light::Serialize(FileStream* stream)
	{
		stream->Write(int(m_lightType));
		stream->Write(m_castShadows);
		stream->Write(m_color);
		stream->Write(m_range);
		stream->Write(m_intensity);
		stream->Write(m_angle);
		stream->Write(m_bias);
	}

	void Light::Deserialize(FileStream* stream)
	{
		SetLightType(LightType(stream->ReadInt()));
		stream->Read(&m_castShadows);
		stream->Read(&m_color);
		stream->Read(&m_range);
		stream->Read(&m_intensity);
		stream->Read(&m_angle);
		stream->Read(&m_bias);
	}

	void Light::SetLightType(LightType type)
	{
		m_lightType = type;
		m_isDirty = true;
		ShadowMap_Create(true);
	}

	void Light::SetCastShadows(bool castShadows)
	{
		if (m_castShadows = castShadows)
			return;

		m_castShadows = castShadows;
		ShadowMap_Create(true);
	}

	void Light::SetRange(float range)
	{
		m_range = Clamp(range, 0.0f, INFINITY);
		m_isDirty = true;
	}

	void Light::SetAngle(float angle)
	{
		m_angle = Clamp(angle, 0.0f, 1.0f);
		m_isDirty = true;
	}

	Vector3 Light::GetDirection()
	{
		return GetTransform()->GetForward();
	}

	void Light::ClampRotation()
	{
		Vector3 rotation = GetTransform()->GetRotation().ToEulerAngles();
		if (rotation.x <= 0.0f)
		{
			GetTransform()->SetRotation(Quaternion::FromEulerAngles(179.0f, rotation.y, rotation.z));
		}
		if (rotation.x >= 180.0f)
		{
			GetTransform()->SetRotation(Quaternion::FromEulerAngles(1.0f, rotation.y, rotation.z));
		}
	}

	void Light::ComputeViewMatrix()
	{
		Vector3 lightDirection	= GetDirection();
		Vector3 position		= lightDirection;
		Vector3 lookAt			= position + lightDirection;
		Vector3 up				= Vector3::Up;

		// Create the view matrix
		m_viewMatrix = Matrix::CreateLookAtLH(position, lookAt, up);
	}

	bool Light::IsInViewFrustrum(Renderable* renderable, unsigned int index  /*= 0*/)
	{
		BoundingBox box = renderable->Geometry_BB();
		Vector3 center	= box.GetCenter();
		Vector3 extents = box.GetExtents();

		return m_frustums[index]->CheckCube(center, extents) != Outside;
	}

	const Matrix& Light::ShadowMap_GetProjectionMatrix(unsigned int index /*= 0*/)
	{
		if (index >= (unsigned int)m_shadowMapsProjectionMatrix.size())
			return Matrix::Identity;

		return m_shadowMapsProjectionMatrix[index];
	}

	shared_ptr<RHI_RenderTexture> Light::ShadowMap_GetRenderTexture(unsigned int index /*= 0*/)
	{
		if (index >= (unsigned int)m_shadowMaps.size())
			return nullptr;

		return m_shadowMaps[index];
	}

	float Light::ShadowMap_GetSplit(unsigned int index /*= 0*/)
	{
		if (index >= (unsigned int)m_shadowMapSplits.size())
			return 0.0f;

		return m_shadowMapSplits[index];
	}

	void Light::ShadowMap_SetSplit(float split, unsigned int index /*= 0*/)
	{
		if (index >= (unsigned int)m_shadowMapSplits.size())
			return;

		m_shadowMapSplits[index] = split;
	}

	shared_ptr<Frustum> Light::ShadowMap_IsInViewFrustrum(unsigned int index /*= 0*/)
	{
		if (index >= (unsigned int)m_frustums.size())
			return nullptr;

		return m_frustums[index];
	}

	void Light::ShadowMap_ComputeProjectionMatrix(unsigned int index /*= 0*/)
	{
		// Hardcoded sizes to match the splits
		float extents = 0;
		if (index == 0)
			extents = 10;

		if (index == 1)
			extents = 45;

		if (index == 2)
			extents = 90;

		Vector3 center = m_lastPosCamera * m_viewMatrix;
		Vector3 min = center - Vector3(extents, extents, extents);
		Vector3 max = center + Vector3(extents, extents, extents);

		//= Shadow shimmering remedy based on ============================================
		// https://msdn.microsoft.com/en-us/library/windows/desktop/ee416324(v=vs.85).aspx
		float fWorldUnitsPerTexel = (extents * 2.0f) / m_shadowMapResolution;

		min /= fWorldUnitsPerTexel;
		min.Floor();
		min *= fWorldUnitsPerTexel;
		max /= fWorldUnitsPerTexel;
		max.Floor();
		max *= fWorldUnitsPerTexel;
		//================================================================================

		m_shadowMapsProjectionMatrix[index] = Matrix::CreateOrthoOffCenterLH(min.x, max.x, min.y, max.y, min.z, max.z);
	}

	void Light::ShadowMap_Create(bool force)
	{		
		if (!force && !m_shadowMaps.empty())
			return;

		ShadowMap_Destroy();

		// Compute shadow map count
		if (GetLightType() == LightType_Directional)
		{
			m_shadowMapCount = 3; // cascades
		}
		else if (GetLightType() == LightType_Point)
		{
			m_shadowMapCount = 6; // points of view
		}
		else if (GetLightType() == LightType_Spot)
		{
			m_shadowMapCount = 1;
		}

		// Create the shadow maps
		m_shadowMapResolution	= Settings::Get().Shadows_GetResolution();
		auto rhiDevice			= m_context->GetSubsystem<Renderer>()->GetRHIDevice();
		for (unsigned int i = 0; i < m_shadowMapCount; i++)
		{
			m_shadowMaps.emplace_back(make_unique<RHI_RenderTexture>(rhiDevice, m_shadowMapResolution, m_shadowMapResolution, Texture_Format_R32_FLOAT, true, Texture_Format_D32_FLOAT)); // could use the g-buffers depth which should be same res
			m_frustums.emplace_back(make_shared<Frustum>());
		}
	}

	void Light::ShadowMap_Destroy()
	{
		m_shadowMaps.clear();
		m_shadowMaps.shrink_to_fit();

		m_frustums.clear();
		m_frustums.shrink_to_fit();
	}
}
