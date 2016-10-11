/*
Copyright(c) 2016 Panos Karabelas

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

//= INCLUDES ===========================
#include "Renderer.h"
#include "../Core/Helper.h"
#include "../Core/Settings.h"
#include "Shaders/PostProcessShader.h"
#include "Shaders/DebugShader.h"
#include "Shaders/DepthShader.h"
#include "Shaders/DeferredShader.h"
#include "../Core/GameObject.h"
#include "../Components/Transform.h"
#include "../Components/MeshRenderer.h"
#include "../Components/LineRenderer.h"
#include "../Physics/PhysicsWorld.h"
#include "../Physics/PhysicsDebugDraw.h"
#include "D3D11/D3D11RenderTexture.h"
#include "../Core/Scene.h"
#include "../IO/Log.h"
#include "../Components/MeshFilter.h"
#include "../Signals/Signaling.h"
//======================================

//= NAMESPACES ================
using namespace std;
using namespace Directus::Math;
//=============================

Renderer::Renderer()
{
	m_graphics = nullptr;
	m_GBuffer = nullptr;
	m_fullScreenQuad = nullptr;
	m_renderedMeshesCount = 0;
	m_meshesRendered = 0;
	m_renderTexPing = nullptr;
	m_renderTexPong = nullptr;
	m_shaderDeferred = nullptr;
	m_shaderDepth = nullptr;
	m_shaderDebug = nullptr;
	m_shaderFXAA = nullptr;
	m_shaderSharpening = nullptr;
	m_texNoiseMap = nullptr;
	m_frustrum = nullptr;
	m_skybox = nullptr;
	m_physics = nullptr;
	m_scene = nullptr;
	m_timer = nullptr;
	m_camera = nullptr;
	m_texEnvironment = nullptr;
	m_lineRenderer = nullptr;
	m_directionalLight = nullptr;
	m_nearPlane = 0.0;
	m_farPlane = 0.0f;
}

Renderer::~Renderer()
{
	// misc
	SafeDelete(m_frustrum);
	SafeDelete(m_fullScreenQuad);
	SafeDelete(m_GBuffer);

	// shaders
	SafeDelete(m_shaderDeferred);
	SafeDelete(m_shaderDepth);
	SafeDelete(m_shaderDebug);
	SafeDelete(m_shaderFXAA);
	SafeDelete(m_shaderSharpening);

	// textures
	SafeDelete(m_renderTexPing);
	SafeDelete(m_renderTexPong);
}

void Renderer::Initialize(Graphics* d3d11device, Timer* timer, PhysicsWorld* physics, Scene* scene, ShaderPool* shaderPool, MaterialPool* materialPool)
{
	m_timer = timer;
	m_physics = physics;
	m_scene = scene;
	m_shaderPool = shaderPool;
	m_materialPool = materialPool;

	m_graphics = d3d11device;

	m_GBuffer = new GBuffer(m_graphics);
	m_GBuffer->Initialize(RESOLUTION_WIDTH, RESOLUTION_HEIGHT);

	m_frustrum = new Frustrum();

	m_fullScreenQuad = new FullScreenQuad;
	m_fullScreenQuad->Initialize(RESOLUTION_WIDTH, RESOLUTION_HEIGHT, m_graphics);

	/*------------------------------------------------------------------------------
									[SHADERS]
	------------------------------------------------------------------------------*/
	m_shaderDeferred = new DeferredShader();
	m_shaderDeferred->Initialize(m_graphics);

	m_shaderDepth = new DepthShader();
	m_shaderDepth->Initialize(m_graphics);

	m_shaderDebug = new DebugShader();
	m_shaderDebug->Initialize(m_graphics);

	m_shaderFXAA = new PostProcessShader();
	m_shaderFXAA->Initialize("FXAA", m_graphics);

	m_shaderSharpening = new PostProcessShader();
	m_shaderSharpening->Initialize("SHARPENING", m_graphics);

	/*------------------------------------------------------------------------------
								[RENDER TEXTURES]
	------------------------------------------------------------------------------*/
	m_renderTexPing = new D3D11RenderTexture;
	m_renderTexPing->Initialize(m_graphics, RESOLUTION_WIDTH, RESOLUTION_HEIGHT);

	m_renderTexPong = new D3D11RenderTexture;
	m_renderTexPong->Initialize(m_graphics, RESOLUTION_WIDTH, RESOLUTION_HEIGHT);

	/*------------------------------------------------------------------------------
										[MISC]
	------------------------------------------------------------------------------*/
	m_texNoiseMap = make_shared<Texture>();
	m_texNoiseMap->LoadFromFile("Assets/Shaders/noise.png");
	m_texNoiseMap->SetType(Normal);
}

void Renderer::Render()
{
	EMIT_SIGNAL(SIGNAL_RENDER_START);

	StartCalculatingStats();
	AcquirePrerequisites();

	if (!m_camera)
	{
		m_graphics->Clear(Vector4(0, 0, 0, 1));
		m_graphics->Present();
		return;
	}

	if (m_renderables.empty())
	{
		m_graphics->Clear(m_camera->GetClearColor());
		m_graphics->Present();
		return;
	}

	// ENABLE Z-BUFFER
	m_graphics->EnableZBuffer(true);

	// Render light depth
	if (m_directionalLight)
		if (m_directionalLight->GetShadowType() != No_Shadows)
			DirectionalLightDepthPass();

	// G-Buffer Construction
	m_GBuffer->SetRenderTargets();
	m_GBuffer->Clear(m_camera->GetClearColor());
	GBufferPass();

	// DISABLE Z BUFFER - SET FULLSCREEN QUAD
	m_graphics->EnableZBuffer(false);
	m_fullScreenQuad->SetBuffers();

	// Deferred Pass
	DeferredPass();

	// Post Proessing
	PostProcessing();

	// Gizmos
	if (GET_ENGINE_MODE == Editor_Idle)
		Gizmos();

	// display frame
	m_graphics->Present();

	StopCalculatingStats();

	EMIT_SIGNAL(SIGNAL_RENDER_END);
}

void Renderer::SetResolution(int width, int height)
{
	// A resolution of 0 won'tcause a crash or anything crazy,
	// but it will cause the depth stancil buffer creation to fail,
	// various error messages to be displayed. I silently prevent that.
	if (width == 0 || height == 0)
		return;

	SET_RESOLUTION(width, height);

	m_graphics->SetViewport(width, height);

	SafeDelete(m_GBuffer);
	m_GBuffer = new GBuffer(m_graphics);
	m_GBuffer->Initialize(RESOLUTION_WIDTH, RESOLUTION_HEIGHT);

	SafeDelete(m_fullScreenQuad);
	m_fullScreenQuad = new FullScreenQuad;
	m_fullScreenQuad->Initialize(RESOLUTION_WIDTH, RESOLUTION_HEIGHT, m_graphics);

	SafeDelete(m_renderTexPing);
	m_renderTexPing = new D3D11RenderTexture;
	m_renderTexPing->Initialize(m_graphics, RESOLUTION_WIDTH, RESOLUTION_HEIGHT);

	SafeDelete(m_renderTexPong);
	m_renderTexPong = new D3D11RenderTexture;
	m_renderTexPong->Initialize(m_graphics, RESOLUTION_WIDTH, RESOLUTION_HEIGHT);
}

void Renderer::Clear()
{
	m_renderables.clear();
	m_renderables.shrink_to_fit();

	m_lightsDirectional.clear();
	m_lightsDirectional.shrink_to_fit();

	m_lightsPoint.clear();
	m_lightsPoint.shrink_to_fit();
}

void Renderer::Update(const vector<GameObject*>& renderables, const vector<GameObject*>& lightsDirectional, const vector<GameObject*>& lightsPoint)
{
	Clear();

	m_renderables = renderables;
	m_lightsDirectional = lightsDirectional;
	m_lightsPoint = lightsPoint;
}

const vector<GameObject*>& Renderer::GetRenderables() const
{
	return m_renderables;
}

void Renderer::AcquirePrerequisites()
{
	GameObject* camera = m_scene->GetMainCamera();
	if (camera)
	{
		m_camera = camera->GetComponent<Camera>();

		GameObject* skybox = m_scene->GetSkybox();
		if (skybox)
		{
			m_skybox = skybox->GetComponent<Skybox>();
			m_lineRenderer = skybox->GetComponent<LineRenderer>(); // Hush hush...
		}

		if (m_lightsDirectional.size() != 0)
			m_directionalLight = m_lightsDirectional[0]->GetComponent<Light>();
		else
			m_directionalLight = nullptr;

		mProjection = m_camera->GetProjectionMatrix();
		mOrthographicProjection = m_camera->GetOrthographicProjectionMatrix();
		mView = m_camera->GetViewMatrix();
		mBaseView = m_camera->GetBaseViewMatrix();
		m_nearPlane = m_camera->GetNearPlane();
		m_farPlane = m_camera->GetFarPlane();
	}
	else
	{
		m_camera = nullptr;
		m_skybox = nullptr;
		m_lineRenderer = nullptr;
		m_directionalLight = nullptr;
	}
}

void Renderer::DirectionalLightDepthPass()
{
	m_graphics->SetCullMode(CullFront);	

	for (int cascadeIndex = 0; cascadeIndex < m_directionalLight->GetCascadeCount(); cascadeIndex++)
	{
		m_directionalLight->SetShadowMapAsRenderTarget(cascadeIndex);
		for (GameObject* gameObject : m_renderables)
		{
			MeshRenderer* meshRenderer = gameObject->GetComponent<MeshRenderer>();
			MeshFilter* meshFilter = gameObject->GetComponent<MeshFilter>();
			Mesh* mesh = meshFilter->GetMesh();

			if (!mesh)
				continue;

			// Not all gameobjects should cast shadows...
			if (gameObject->GetComponent<Skybox>() || !meshRenderer->GetCastShadows())
				continue;

			if (meshFilter->SetBuffers())
			{
				m_shaderDepth->Render(
					mesh->GetIndexCount(),
					gameObject->GetTransform()->GetWorldTransform(),
					m_directionalLight->GetViewMatrix(),
					m_directionalLight->GetOrthographicProjectionMatrix(cascadeIndex)
				);
			}
		}
	}
}

void Renderer::GBufferPass()
{
	for (ShaderVariation* currentShader : m_shaderPool->GetAllShaders()) // for each shader
	{	
		currentShader->Set();
		for (Material* currentMaterial : m_materialPool->GetAllMaterials()) // for each material that uses this shader
		{
			if (currentMaterial->GetShader()->GetID() != currentShader->GetID())
				continue;	

			m_textures.push_back(currentMaterial->GetShaderResourceViewByTextureType(Albedo));
			m_textures.push_back(currentMaterial->GetShaderResourceViewByTextureType(Roughness));
			m_textures.push_back(currentMaterial->GetShaderResourceViewByTextureType(Metallic));
			m_textures.push_back(currentMaterial->GetShaderResourceViewByTextureType(Occlusion));
			m_textures.push_back(currentMaterial->GetShaderResourceViewByTextureType(Normal));
			m_textures.push_back(currentMaterial->GetShaderResourceViewByTextureType(Height));
			m_textures.push_back(currentMaterial->GetShaderResourceViewByTextureType(Mask));
			if (m_directionalLight)
			{
				for (int i = 0; i < m_directionalLight->GetCascadeCount(); i++)
					m_textures.push_back(m_directionalLight->GetDepthMap(i));
			}
			else
				m_textures.push_back(nullptr);

			currentShader->SetResources(m_textures);

			for (GameObject* gameObject : m_renderables) // for each mesh that uses this material:
			{
				//= Get all that we need ===================================================
				MeshFilter* meshFilter = gameObject->GetComponent<MeshFilter>();
				Mesh* mesh = meshFilter->GetMesh();
				MeshRenderer* meshRenderer = gameObject->GetComponent<MeshRenderer>();
				Material* material = meshRenderer->GetMaterial();
				Matrix mWorld = gameObject->GetTransform()->GetWorldTransform();
				//==========================================================================

				// If any rendering requirement is missing, skip this GameObject
				if (!meshFilter || !mesh || !meshRenderer || !material)
					continue;

				if (currentMaterial->GetID() != material->GetID())
					continue;

				// Skip transparent meshes
				if (material->GetOpacity() < 1.0f)
					continue;

				// Frustrum culling
				if (!IsInViewFrustrum(meshFilter->GetCenter(), meshFilter->GetBoundingBox()))
					continue;

				currentShader->SetBuffers(mWorld, mView, mProjection, currentMaterial, m_directionalLight, meshRenderer->GetReceiveShadows(), m_camera);
				
				// Set mesh buffer
				if (meshFilter->SetBuffers())
				{
					// Set face culling
					m_graphics->SetCullMode(material->GetFaceCullMode());

					//= Render =================================================================					
					meshRenderer->Render(mesh->GetIndexCount());
					m_meshesRendered++;
					//==========================================================================
				}
			}

			m_textures.clear();
			m_textures.shrink_to_fit();
		}
	}
}

void Renderer::DeferredPass()
{
	if (!m_shaderDeferred->IsCompiled())
		return;

	Ping();

	// Setting a texture array instead of multiple textures is faster
	m_texArrayDeferredPass.clear();
	m_texArrayDeferredPass.shrink_to_fit();
	m_texArrayDeferredPass.push_back(m_GBuffer->GetShaderResourceView(0)); // albedo
	m_texArrayDeferredPass.push_back(m_GBuffer->GetShaderResourceView(1)); // normal
	m_texArrayDeferredPass.push_back(m_GBuffer->GetShaderResourceView(2)); // depth
	m_texArrayDeferredPass.push_back(m_GBuffer->GetShaderResourceView(3)); // material
	m_texArrayDeferredPass.push_back(m_texNoiseMap->GetID3D11ShaderResourceView());

	if (m_skybox)
		m_texEnvironment = m_skybox->GetEnvironmentTexture();
	else
		m_texEnvironment = nullptr;

	// deferred rendering
	m_shaderDeferred->Render(
		m_fullScreenQuad->GetIndexCount(),
		Matrix::Identity,
		mView,
		mBaseView,
		mProjection,
		mOrthographicProjection,
		m_lightsDirectional,
		m_lightsPoint,
		m_camera,
		m_texArrayDeferredPass,
		m_texEnvironment
	);
}

void Renderer::PostProcessing() const
{
	Pong();

	// fxaa pass
	m_shaderFXAA->Render(
		m_fullScreenQuad->GetIndexCount(),
		Matrix::Identity,
		mBaseView,
		mOrthographicProjection,
		m_renderTexPing->GetShaderResourceView()
	);

	m_graphics->ResetRenderTarget();
	m_graphics->ResetViewport();
	m_graphics->Clear(m_camera->GetClearColor());

	// sharpening pass
	m_shaderSharpening->Render(
		m_fullScreenQuad->GetIndexCount(),
		Matrix::Identity,
		mBaseView,
		mOrthographicProjection,
		m_renderTexPong->GetShaderResourceView()
	);
}

void Renderer::Gizmos() const
{
	if (!m_lineRenderer)
		return;

	if (!m_physics->GetPhysicsDebugDraw()->IsDirty())
		return;

	// Pass the line list from bullet to the line renderer component
	m_lineRenderer->AddLineList(m_physics->GetPhysicsDebugDraw()->GetLines());

	// Set the buffer
	m_lineRenderer->SetBuffer();

	// Render
	m_shaderDebug->Render(
		m_lineRenderer->GetVertexCount(),
		Matrix::Identity,
		m_camera->GetViewMatrix(),
		m_camera->GetProjectionMatrix(),
		m_GBuffer->GetShaderResourceView(2) // depth
	);
}

void Renderer::Ping() const
{
	Vector4 clearColor = m_camera ? m_camera->GetClearColor() : Vector4(0.0f, 0.0f, 0.0f, 1.0f);

	m_renderTexPing->SetAsRenderTarget(); // Set the render target to be the render to texture. 
	m_renderTexPing->Clear(clearColor); // Clear the render to texture.
}

void Renderer::Pong() const
{
	Vector4 clearColor = m_camera ? m_camera->GetClearColor() : Vector4(0.0f, 0.0f, 0.0f, 1.0f);

	m_renderTexPong->SetAsRenderTarget(); // Set the render target to be the render to texture. 
	m_renderTexPong->Clear(clearColor); // Clear the render to texture.
}

bool Renderer::IsInViewFrustrum(const Vector3& center, const Vector3& extent)
{
	//= Frustrum culling =======================================================
	if (m_frustrum->GetProjectionMatrix() != mProjection || m_frustrum->GetViewMatrix() != mView)
	{
		m_frustrum->SetProjectionMatrix(mProjection);
		m_frustrum->SetViewMatrix(mView);
		m_frustrum->ConstructFrustum(m_farPlane);
	}

	float radius = max(abs(extent.x), abs(extent.y));
	radius = max(radius, abs(extent.z));

	if (m_frustrum->CheckSphere(center, radius) == Outside)
		return false;

	return true;
}

//= STATS ============================
void Renderer::StartCalculatingStats()
{
	m_meshesRendered = 0;
}

void Renderer::StopCalculatingStats()
{
	// meshes rendered
	m_renderedMeshesCount = m_meshesRendered;
}

int Renderer::GetRenderedMeshesCount() const
{
	return m_renderedMeshesCount;
}
//====================================