#include <iostream>
#include <chrono>

#include <SDL.h>
#include <SDL_syswm.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <core/view.hpp>
#include <render/render_system.hpp>
#include <gameplay/gameplay.hpp>

int main(int, char* [])
{
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);

    auto renderSystemRV = Ride::RenderSystem::Create();
    if (renderSystemRV.result != Ride::GraphicsResult::Ok)
    {
        printf("Failed to create RenderSystem");
        return EXIT_FAILURE;
    }
    std::unique_ptr<Ride::RenderSystem> renderSystem = std::move(renderSystemRV.value);

    auto view = std::make_unique<Ride::View>();
    auto gameplay = std::make_unique<Ride::Gameplay>(std::move(view));

    std::chrono::time_point startTime = std::chrono::high_resolution_clock::now();
    double prevTime = -1.0;
    double curTime = -1.0;
    double deltaTime = -1.0;

    bool run = true;
    while (run)
    {
        SDL_Event evt;
        while (SDL_PollEvent(&evt))
        {
            if (evt.type == SDL_QUIT)
            {
                run = false;
            }
            if (evt.type == SDL_KEYDOWN)
            {
                printf("Keydown\n");
            }
        }

        std::chrono::time_point currentTime = std::chrono::high_resolution_clock::now();

        prevTime = curTime;
        curTime = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime).count() / 1000.0;
        if (prevTime > 0.0 && curTime > 0.0)
        {
            deltaTime = curTime - prevTime;
        }

        const auto& viewportExtent = renderSystem->GetViewportExtent();

        gameplay->SetViewportExtent(viewportExtent.width, viewportExtent.height);
        gameplay->Update(curTime, deltaTime);

        renderSystem->Draw(gameplay->GetView(), gameplay->GetActiveCamera());
    }

    renderSystem.reset();
    view.reset();
    gameplay.reset();

    SDL_Quit();

    return EXIT_SUCCESS;
}
