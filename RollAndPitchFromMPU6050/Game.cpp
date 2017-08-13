//
// Game.cpp
//

#include "pch.h"
#include "Game.h"

#include "imgui.h"
#include "imgui_impl_dx12.h"

using Microsoft::WRL::ComPtr;
using namespace Platform;
using namespace Windows::Foundation;
using namespace DirectX;
using namespace DirectX::SimpleMath;

extern void ExitGame();

Game::Game() :
    m_window(nullptr),
    m_outputWidth(800),
    m_outputHeight(600),
    m_outputRotation(DXGI_MODE_ROTATION_IDENTITY),
    m_featureLevel(D3D_FEATURE_LEVEL_11_0),
    m_backBufferIndex(0),
    m_fenceValues{},
	m_AccelerometerReads(0)
{
}

// Initialize the Direct3D resources required to run.
void Game::Initialize(IUnknown* window, int width, int height, DXGI_MODE_ROTATION rotation)
{
    m_window = window;
    m_outputWidth = std::max(width, 1);
    m_outputHeight = std::max(height, 1);
    m_outputRotation = rotation;

    CreateDevice();
    CreateResources();

	auto initMPU6050Task = InitMPU6050();

	initMPU6050Task.then([this](bool _i2cDeviceFound) {

		if (!_i2cDeviceFound)
		{
			return;	// I2C device not found. Quit.
		}

		// start periodical timer
		TimeSpan timerPeriod;
		timerPeriod.Duration = 40 * 10000; // read MPU6050 accelerometer data every 40 mS

		unsigned char regAddrBuf[]{ 0x3B };	// MPU6050 read data register
		unsigned char readBufChar[14];

		m_ReadRegAddr = ref new Platform::Array<byte>(regAddrBuf, _countof(regAddrBuf));
		m_ReadBuf = ref new Platform::Array<byte>(readBufChar, _countof(readBufChar));

		m_PeriodicTimer = Threading::ThreadPoolTimer::CreatePeriodicTimer(
			ref new Threading::TimerElapsedHandler(
				[this](Threading::ThreadPoolTimer ^timer)
		{
			if (m_I2cMPU6050Device)
			{

				try
				{
					// 1) read MPU6050 sensor datadata
					m_I2cMPU6050Device->WriteRead(m_ReadRegAddr, m_ReadBuf);

					// 2) calculatoins
					short AccelerationRawX = (short)(m_ReadBuf[0] * 256);
					AccelerationRawX += (m_ReadBuf[1]);
					short AccelerationRawY = (short)(m_ReadBuf[2] * 256);
					AccelerationRawY += m_ReadBuf[3];
					short AccelerationRawZ = (short)(m_ReadBuf[4] * 256);
					AccelerationRawZ += m_ReadBuf[5];

					short GyroRawX = (short)(m_ReadBuf[8] * 256);
					GyroRawX += (m_ReadBuf[9]);
					short GyroRawY = (short)(m_ReadBuf[10] * 256);
					GyroRawY += m_ReadBuf[11];
					short GyroRawZ = (short)(m_ReadBuf[12] * 256);
					GyroRawZ += m_ReadBuf[13];

					// accelerometer
					// Convert raw accelerometer values to G's
					const int ACCEL_RES = 32767;	// MPU6050 accelerometer dynamic range = 16 bits signed
					const int ACCEL_DYN_RANGE_G = 2;	// use +/- 2g mode (see register 0x1C)
					const int UNITS_PER_G = ACCEL_RES / ACCEL_DYN_RANGE_G;

					// normalize accelerometer values to +/- 1.0
					float accelX, accelY, accelZ;
					accelX = (float)AccelerationRawX / UNITS_PER_G;
					accelY = (float)AccelerationRawY / UNITS_PER_G;
					accelZ = (float)AccelerationRawZ / UNITS_PER_G;

					// 3) store values to render data
					m_AccelData.accelX = accelX;
					m_AccelData.accelY = accelY;
					m_AccelData.accelZ = accelZ;

					m_AccelerometerReads += 1;
				}
				catch (...)
				{
				}
			}

		})
			, timerPeriod
		);	// of CreatePeriodicTimer

	});	// of initMPU6050Task 'then'
}

// Executes the basic game loop.
void Game::Tick()
{
    m_timer.Tick([&]()
    {
        Update(m_timer);
    });

    Render();
}



// Updates the world.
void Game::Update(DX::StepTimer const& timer)
{
    float elapsedTime = float(timer.GetElapsedSeconds());

	// use MPU6050 accelerometer data in render
	m_AngleRoll = m_AccelData.accelY;
	m_AnglePitch = -m_AccelData.accelX;

	// calculate model rotation matrix
	m_world = Matrix::CreateFromYawPitchRoll(0.0f, m_AnglePitch, m_AngleRoll);
}



// Draws the scene.
void Game::Render()
{
    // Don't try to render anything before the first Update.
    if (m_timer.GetFrameCount() == 0)
    {
        return;
    }

    // Prepare the command list to render a new frame.
    Clear();


	// model DirectXTK
	Model::UpdateEffectMatrices(m_modelNormal, m_world, m_view, m_proj);
	m_model->Draw(m_commandList.Get(), m_modelNormal.cbegin());


	// imgui - show render details
	ImGui_ImplDX12_NewFrame(m_commandList.Get(), m_outputWidth, m_outputHeight);

	constexpr float INFO_WINDOW_WIDTH = 260.0f;
	constexpr float INFO_WINDOW_HEIGHT = 80.0f;

	// put debug window at center bottom position
	ImGui::SetNextWindowPos(ImVec2((0) / 2, m_outputHeight - INFO_WINDOW_HEIGHT), ImGuiSetCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(INFO_WINDOW_WIDTH, INFO_WINDOW_HEIGHT), ImGuiSetCond_FirstUseEver);

	// put data to display
	ImGui::Begin("Performance");
	ImGui::Text("FPS=%.1f", ImGui::GetIO().Framerate);
	ImGui::Text("Accel reads/sec %.1f", float(m_AccelerometerReads / m_timer.GetTotalSeconds()));
	ImGui::End();

	// put debug window at center bottom position
	ImGui::SetNextWindowPos(ImVec2(m_outputWidth - INFO_WINDOW_WIDTH, m_outputHeight - INFO_WINDOW_HEIGHT), ImGuiSetCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(INFO_WINDOW_WIDTH, INFO_WINDOW_HEIGHT), ImGuiSetCond_FirstUseEver);

	// put data to display
	ImGui::Begin("Accelerometer");
	ImGui::SliderFloat("Roll angle", &m_AngleRoll, -1.0f, 1.0f);
	ImGui::SliderFloat("Pitch angle", &m_AnglePitch, -1.0f, 1.0f);
	ImGui::End();

	// render debug window
	m_commandList.Get()->SetDescriptorHeaps(1, g_pd3dSrvDescHeap.GetAddressOf());
	ImGui::Render();


    // Show the new frame.
    Present();
	m_graphicsMemory->Commit(m_commandQueue.Get());
}

// Helper method to prepare the command list for rendering and clear the back buffers.
void Game::Clear()
{
    // Reset command list and allocator.
    DX::ThrowIfFailed(m_commandAllocators[m_backBufferIndex]->Reset());
    DX::ThrowIfFailed(m_commandList->Reset(m_commandAllocators[m_backBufferIndex].Get(), nullptr));

    // Transition the render target into the correct state to allow for drawing into it.
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_backBufferIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_commandList->ResourceBarrier(1, &barrier);

    // Clear the views.
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvDescriptor(m_rtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), m_backBufferIndex, m_rtvDescriptorSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvDescriptor(m_dsvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    m_commandList->OMSetRenderTargets(1, &rtvDescriptor, FALSE, &dsvDescriptor);
    m_commandList->ClearRenderTargetView(rtvDescriptor, Colors::CornflowerBlue, 0, nullptr);
    m_commandList->ClearDepthStencilView(dsvDescriptor, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // Set the viewport and scissor rect.
    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(m_outputWidth), static_cast<float>(m_outputHeight), D3D12_MIN_DEPTH, D3D12_MAX_DEPTH };
    D3D12_RECT scissorRect = { 0, 0, m_outputWidth, m_outputHeight };
    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissorRect);
}

// Submits the command list to the GPU and presents the back buffer contents to the screen.
void Game::Present()
{
    // Transition the render target to the state that allows it to be presented to the display.
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_backBufferIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->ResourceBarrier(1, &barrier);

    // Send the command list off to the GPU for processing.
    DX::ThrowIfFailed(m_commandList->Close());
    m_commandQueue->ExecuteCommandLists(1, CommandListCast(m_commandList.GetAddressOf()));

    // The first argument instructs DXGI to block until VSync, putting the application
    // to sleep until the next VSync. This ensures we don't waste any cycles rendering
    // frames that will never be displayed to the screen.
    HRESULT hr = m_swapChain->Present(1, 0);

    // If the device was reset we must completely reinitialize the renderer.
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    {
        OnDeviceLost();
    }
    else
    {
        DX::ThrowIfFailed(hr);

        MoveToNextFrame();
    }
}

// Message handlers
void Game::OnActivated()
{
    // TODO: Game is becoming active window.
}

void Game::OnDeactivated()
{
    // TODO: Game is becoming background window.
}

void Game::OnSuspending()
{
    // TODO: Game is being power-suspended.
}

void Game::OnResuming()
{
    m_timer.ResetElapsedTime();

    // TODO: Game is being power-resumed.
}

void Game::OnWindowSizeChanged(int width, int height, DXGI_MODE_ROTATION rotation)
{
    m_outputWidth = std::max(width, 1);
    m_outputHeight = std::max(height, 1);
    m_outputRotation = rotation;

    CreateResources();

    // TODO: Game window is being resized.
}

void Game::ValidateDevice()
{
    // The D3D Device is no longer valid if the default adapter changed since the device
    // was created or if the device has been removed.

    DXGI_ADAPTER_DESC previousDesc;
    {
        ComPtr<IDXGIAdapter1> previousDefaultAdapter;
        DX::ThrowIfFailed(m_dxgiFactory->EnumAdapters1(0, previousDefaultAdapter.GetAddressOf()));

        DX::ThrowIfFailed(previousDefaultAdapter->GetDesc(&previousDesc));
    }

    DXGI_ADAPTER_DESC currentDesc;
    {
        ComPtr<IDXGIFactory4> currentFactory;
        DX::ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(currentFactory.GetAddressOf())));

        ComPtr<IDXGIAdapter1> currentDefaultAdapter;
        DX::ThrowIfFailed(currentFactory->EnumAdapters1(0, currentDefaultAdapter.GetAddressOf()));

        DX::ThrowIfFailed(currentDefaultAdapter->GetDesc(&currentDesc));
    }

    // If the adapter LUIDs don't match, or if the device reports that it has been removed,
    // a new D3D device must be created.

    if (previousDesc.AdapterLuid.LowPart != currentDesc.AdapterLuid.LowPart
        || previousDesc.AdapterLuid.HighPart != currentDesc.AdapterLuid.HighPart
        || FAILED(m_d3dDevice->GetDeviceRemovedReason()))
    {
        // Create a new device and swap chain.
        OnDeviceLost();
    }
}

// Properties
void Game::GetDefaultSize(int& width, int& height) const
{
    // TODO: Change to desired default window size (note minimum size is 320x200).
    width = 800;
    height = 600;
}

// These are the resources that depend on the device.
void Game::CreateDevice()
{
#if defined(_DEBUG)
    // Enable the debug layer (requires the Graphics Tools "optional feature").
    //
    // NOTE: Enabling the debug layer after device creation will invalidate the active device.
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debugController.GetAddressOf()))))
        {
            debugController->EnableDebugLayer();
        }
    }
#endif

    DX::ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(m_dxgiFactory.ReleaseAndGetAddressOf())));

    ComPtr<IDXGIAdapter1> adapter;
    GetAdapter(adapter.GetAddressOf());

    // Create the DX12 API device object.
    DX::ThrowIfFailed(D3D12CreateDevice(
        adapter.Get(),
        m_featureLevel,
        IID_PPV_ARGS(m_d3dDevice.ReleaseAndGetAddressOf())
        ));

#ifndef NDEBUG
    // Configure debug device (if active).
    ComPtr<ID3D12InfoQueue> d3dInfoQueue;
    if (SUCCEEDED(m_d3dDevice.As(&d3dInfoQueue)))
    {
#ifdef _DEBUG
        d3dInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
        d3dInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
#endif
        D3D12_MESSAGE_ID hide[] =
        {
            D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,
            D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE
        };
        D3D12_INFO_QUEUE_FILTER filter = {};
        filter.DenyList.NumIDs = _countof(hide);
        filter.DenyList.pIDList = hide;
        d3dInfoQueue->AddStorageFilterEntries(&filter);
    }
#endif

    // Create the command queue.
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    DX::ThrowIfFailed(m_d3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(m_commandQueue.ReleaseAndGetAddressOf())));

    // Create descriptor heaps for render target views and depth stencil views.
    D3D12_DESCRIPTOR_HEAP_DESC rtvDescriptorHeapDesc = {};
    rtvDescriptorHeapDesc.NumDescriptors = c_swapBufferCount;
    rtvDescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

    D3D12_DESCRIPTOR_HEAP_DESC dsvDescriptorHeapDesc = {};
    dsvDescriptorHeapDesc.NumDescriptors = 1;
    dsvDescriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;

    DX::ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&rtvDescriptorHeapDesc, IID_PPV_ARGS(m_rtvDescriptorHeap.ReleaseAndGetAddressOf())));
    DX::ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&dsvDescriptorHeapDesc, IID_PPV_ARGS(m_dsvDescriptorHeap.ReleaseAndGetAddressOf())));

    m_rtvDescriptorSize = m_d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Create a command allocator for each back buffer that will be rendered to.
    for (UINT n = 0; n < c_swapBufferCount; n++)
    {
        DX::ThrowIfFailed(m_d3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(m_commandAllocators[n].ReleaseAndGetAddressOf())));
    }

    // Create a command list for recording graphics commands.
    DX::ThrowIfFailed(m_d3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocators[0].Get(), nullptr, IID_PPV_ARGS(m_commandList.ReleaseAndGetAddressOf())));
    DX::ThrowIfFailed(m_commandList->Close());

    // Create a fence for tracking GPU execution progress.
    DX::ThrowIfFailed(m_d3dDevice->CreateFence(m_fenceValues[m_backBufferIndex], D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.ReleaseAndGetAddressOf())));
    m_fenceValues[m_backBufferIndex]++;

    m_fenceEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
    if (!m_fenceEvent.IsValid())
    {
        throw std::exception("CreateEvent");
    }

	// model DirectXTK
	m_graphicsMemory = std::make_unique<GraphicsMemory>(m_d3dDevice.Get());

	m_states = std::make_unique<CommonStates>(m_d3dDevice.Get());

	m_model = Model::CreateFromSDKMESH(L"Assets/airplane.sdkmesh");	// load the model

	ResourceUploadBatch resourceUpload(m_d3dDevice.Get());

	resourceUpload.Begin();

	m_modelResources = m_model->LoadTextures(m_d3dDevice.Get(), resourceUpload);

	//	m_fxFactory = std::make_unique<EffectFactory>(m_modelResources->Heap(), m_states->Heap());
	m_fxFactory = std::make_unique<EffectFactory>(m_d3dDevice.Get());

	auto uploadResourcesFinished = resourceUpload.End(m_commandQueue.Get());

	uploadResourcesFinished.wait();

	RenderTargetState rtState(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_D32_FLOAT);
	EffectPipelineStateDescription pd(nullptr, CommonStates::Opaque, CommonStates::DepthDefault, CommonStates::CullClockwise, rtState);
	EffectPipelineStateDescription pdAlpha(nullptr, CommonStates::AlphaBlend, CommonStates::DepthDefault, CommonStates::CullClockwise, rtState);

	m_modelNormal = m_model->CreateEffects(*m_fxFactory, pd, pdAlpha);

	m_world = Matrix::Identity;


	// imgui
	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	desc.NumDescriptors = 1;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	m_d3dDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dSrvDescHeap));

	ImGui_ImplDX12_Init(NULL, c_swapBufferCount, m_d3dDevice.Get(), g_pd3dSrvDescHeap.Get()->GetCPUDescriptorHandleForHeapStart(), g_pd3dSrvDescHeap.Get()->GetGPUDescriptorHandleForHeapStart());

}

// Allocate all memory resources that change on a window SizeChanged event.
void Game::CreateResources()
{
    // Wait until all previous GPU work is complete.
    WaitForGpu();

    // Release resources that are tied to the swap chain and update fence values.
    for (UINT n = 0; n < c_swapBufferCount; n++)
    {
        m_renderTargets[n].Reset();
        m_fenceValues[n] = m_fenceValues[m_backBufferIndex];
    }

    DXGI_FORMAT backBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    DXGI_FORMAT depthBufferFormat = DXGI_FORMAT_D32_FLOAT;
    UINT backBufferWidth = static_cast<UINT>(m_outputWidth);
    UINT backBufferHeight = static_cast<UINT>(m_outputHeight);

    // If the swap chain already exists, resize it, otherwise create one.
    if (m_swapChain)
    {
        HRESULT hr = m_swapChain->ResizeBuffers(c_swapBufferCount, backBufferWidth, backBufferHeight, backBufferFormat, 0);

        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        {
            // If the device was removed for any reason, a new device and swap chain will need to be created.
            OnDeviceLost();

            // Everything is set up now. Do not continue execution of this method. OnDeviceLost will reenter this method 
            // and correctly set up the new device.
            return;
        }
        else
        {
            DX::ThrowIfFailed(hr);
        }
    }
    else
    {
        // Create a descriptor for the swap chain.
        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.Width = backBufferWidth;
        swapChainDesc.Height = backBufferHeight;
        swapChainDesc.Format = backBufferFormat;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount = c_swapBufferCount;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.SampleDesc.Quality = 0;
        swapChainDesc.Scaling = DXGI_SCALING_ASPECT_RATIO_STRETCH;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        // Create a swap chain for the window.
        ComPtr<IDXGISwapChain1> swapChain;
        DX::ThrowIfFailed(m_dxgiFactory->CreateSwapChainForCoreWindow(
            m_commandQueue.Get(),
            m_window,
            &swapChainDesc,
            nullptr,
            swapChain.GetAddressOf()
            ));

        DX::ThrowIfFailed(swapChain.As(&m_swapChain));
    }

    DX::ThrowIfFailed(m_swapChain->SetRotation(m_outputRotation));

    // Obtain the back buffers for this window which will be the final render targets
    // and create render target views for each of them.
    for (UINT n = 0; n < c_swapBufferCount; n++)
    {
        DX::ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(m_renderTargets[n].GetAddressOf())));

        wchar_t name[25] = {};
        swprintf_s(name, L"Render target %u", n);
        m_renderTargets[n]->SetName(name);

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvDescriptor(m_rtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), n, m_rtvDescriptorSize);
        m_d3dDevice->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr, rtvDescriptor);
    }

    // Reset the index to the current back buffer.
    m_backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Allocate a 2-D surface as the depth/stencil buffer and create a depth/stencil view
    // on this surface.
    CD3DX12_HEAP_PROPERTIES depthHeapProperties(D3D12_HEAP_TYPE_DEFAULT);

    D3D12_RESOURCE_DESC depthStencilDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        depthBufferFormat,
        backBufferWidth,
        backBufferHeight,
        1, // This depth stencil view has only one texture.
        1  // Use a single mipmap level.
        );
    depthStencilDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE depthOptimizedClearValue = {};
    depthOptimizedClearValue.Format = depthBufferFormat;
    depthOptimizedClearValue.DepthStencil.Depth = 1.0f;
    depthOptimizedClearValue.DepthStencil.Stencil = 0;

    DX::ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
        &depthHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &depthStencilDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &depthOptimizedClearValue,
        IID_PPV_ARGS(m_depthStencil.ReleaseAndGetAddressOf())
        ));

    m_depthStencil->SetName(L"Depth stencil");

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = depthBufferFormat;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

    m_d3dDevice->CreateDepthStencilView(m_depthStencil.Get(), &dsvDesc, m_dsvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

	// model DirectXTK
	m_view = Matrix::CreateLookAt(Vector3(0.f, 4.5f, -24.f), Vector3(0.f, -1.0f, 0.0f), Vector3::UnitY);
	m_proj = Matrix::CreatePerspectiveFieldOfView(XM_PI / 4.f, float(backBufferWidth) / float(backBufferHeight), 0.1f, 1000.f);
}

void Game::WaitForGpu()
{
    // Schedule a Signal command in the GPU queue.
    DX::ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_fenceValues[m_backBufferIndex]));

    // Wait until the Signal has been processed.
    DX::ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_backBufferIndex], m_fenceEvent.Get()));
    WaitForSingleObjectEx(m_fenceEvent.Get(), INFINITE, FALSE);

    // Increment the fence value for the current frame.
    m_fenceValues[m_backBufferIndex]++;
}

void Game::MoveToNextFrame()
{
    // Schedule a Signal command in the queue.
    const UINT64 currentFenceValue = m_fenceValues[m_backBufferIndex];
    DX::ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), currentFenceValue));

    // Update the back buffer index.
    m_backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();

    // If the next frame is not ready to be rendered yet, wait until it is ready.
    if (m_fence->GetCompletedValue() < m_fenceValues[m_backBufferIndex])
    {
        DX::ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_backBufferIndex], m_fenceEvent.Get()));
        WaitForSingleObjectEx(m_fenceEvent.Get(), INFINITE, FALSE);
    }

    // Set the fence value for the next frame.
    m_fenceValues[m_backBufferIndex] = currentFenceValue + 1;
}

// This method acquires the first available hardware adapter that supports Direct3D 12.
// If no such adapter can be found, try WARP. Otherwise throw an exception.
void Game::GetAdapter(IDXGIAdapter1** ppAdapter)
{
    *ppAdapter = nullptr;

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT adapterIndex = 0; DXGI_ERROR_NOT_FOUND != m_dxgiFactory->EnumAdapters1(adapterIndex, adapter.ReleaseAndGetAddressOf()); ++adapterIndex)
    {
        DXGI_ADAPTER_DESC1 desc;
        DX::ThrowIfFailed(adapter->GetDesc1(&desc));

        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            // Don't select the Basic Render Driver adapter.
            continue;
        }

        // Check to see if the adapter supports Direct3D 12, but don't create the actual device yet.
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), m_featureLevel, _uuidof(ID3D12Device), nullptr)))
        {
            break;
        }
    }

#if !defined(NDEBUG)
    if (!adapter)
    {
        if (FAILED(m_dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(adapter.ReleaseAndGetAddressOf()))))
        {
            throw std::exception("WARP12 not available. Enable the 'Graphics Tools' optional feature");
        }
    }
#endif

    if (!adapter)
    {
        throw std::exception("No Direct3D 12 device found");
    }

    *ppAdapter = adapter.Detach();
}

void Game::OnDeviceLost()
{
	// DirectXTK
	m_states.reset();
	m_fxFactory.reset();
	m_modelResources.reset();
	m_model.reset();
	m_modelNormal.clear();
	m_graphicsMemory.reset();

    for (UINT n = 0; n < c_swapBufferCount; n++)
    {
        m_commandAllocators[n].Reset();
        m_renderTargets[n].Reset();
    }

    m_depthStencil.Reset();
    m_fence.Reset();
    m_commandList.Reset();
    m_swapChain.Reset();
    m_rtvDescriptorHeap.Reset();
    m_dsvDescriptorHeap.Reset();
    m_commandQueue.Reset();
    m_d3dDevice.Reset();
    m_dxgiFactory.Reset();

    CreateDevice();
    CreateResources();
}

// write configuration register with I2C
bool Game::WriteByteToI2C(I2cDevice^ _I2cMPU6050Device, byte _regAddr, byte _data)
{
	unsigned char writeBuf[]{ _regAddr, _data };
	Platform::Array<byte> ^wb = ref new Platform::Array<byte>(writeBuf, _countof(writeBuf));

	try
	{
		_I2cMPU6050Device->Write(wb);
		return true;
	}
	catch (...)
	{
		return false;
	}
}

// initialize MPU6050 device at I2C
Concurrency::task<bool> Game::InitMPU6050()
{
	return Concurrency::create_task([this]()
	{
		String^ i2cDeviceSelector = I2cDevice::GetDeviceSelector();

		return Concurrency::create_task(DeviceInformation::FindAllAsync(i2cDeviceSelector)).then([this](DeviceInformationCollection^ devices)
		{
			const unsigned int I2C_ADDRESS = 0x68;	// I2C address for MPU6050 sensor

			if (devices->Size == 0)
			{
				return Concurrency::task_from_result(false);
			}
			else
			{
				auto HTU21D_settings = ref new I2cConnectionSettings(I2C_ADDRESS);

				return Concurrency::create_task(I2cDevice::FromIdAsync(devices->GetAt(0)->Id, HTU21D_settings)).then([this](I2cDevice^ i2cDevice) {

					if (i2cDevice)
					{
						m_I2cMPU6050Device = i2cDevice;	// save active I2C device

						// init accelerometer
						if (m_I2cMPU6050Device)
						{
							// see MPU-6000-Register-Map1.pdf for registers details

							// init MPU6050
							if (!WriteByteToI2C(m_I2cMPU6050Device, 0x6B, 0x80))
							{
								return false;
							}
							::Sleep(100);
							if (!WriteByteToI2C(m_I2cMPU6050Device, 0x6B, 0x2))
							{
								return false;
							}

							if (!WriteByteToI2C(m_I2cMPU6050Device, 0x1A, 4))		// Accelerometer= 21Hz
							{
								return false;
							}
							if (!WriteByteToI2C(m_I2cMPU6050Device, 0x1C, 0))		// Accelerometer= +/- 2g
							{
								return false;
							}
						}
						else
						{
							return false;
						}

						return true;
					}
					else
					{
						m_I2cMPU6050Device = nullptr;	// no I2C device found
						return false;
					}
				});
			}
		});
	});
}
