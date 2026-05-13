/*
 * gl_test — Minimal GL query test using BGLView.
 * Prints GL strings to stdout to verify crocus GL context works.
 */
#include <stdio.h>
#include <Application.h>
#include <Window.h>
#include <GLView.h>
#include <GL/gl.h>

class GLTestWindow : public BWindow {
public:
	GLTestWindow()
		: BWindow(BRect(100, 100, 400, 300), "GL Test",
			B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE)
	{
		fGLView = new BGLView(Bounds(), "gl",
			B_FOLLOW_ALL, B_WILL_DRAW,
			BGL_RGB | BGL_ALPHA | BGL_DOUBLE);
		AddChild(fGLView);
	}

	void QueryGL()
	{
		fGLView->LockGL();

		const char* vendor = (const char*)glGetString(GL_VENDOR);
		const char* renderer = (const char*)glGetString(GL_RENDERER);
		const char* version = (const char*)glGetString(GL_VERSION);
		const char* glsl = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);

		printf("=== GL Query Results ===\n");
		printf("GL_VENDOR:   %s\n", vendor ? vendor : "(null)");
		printf("GL_RENDERER: %s\n", renderer ? renderer : "(null)");
		printf("GL_VERSION:  %s\n", version ? version : "(null)");
		printf("GL_SHADING:  %s\n", glsl ? glsl : "(null)");

		GLenum err = glGetError();
		printf("glGetError:  0x%x\n", err);

		GLint major = 0, minor = 0;
		glGetIntegerv(GL_MAJOR_VERSION, &major);
		glGetIntegerv(GL_MINOR_VERSION, &minor);
		printf("GL version:  %d.%d\n", major, minor);

		// Quick render test: clear to blue
		printf("\n--- Render Test ---\n");
		GLint vp[4];
		glGetIntegerv(GL_VIEWPORT, vp);
		printf("Viewport: %d,%d %dx%d\n", vp[0], vp[1], vp[2], vp[3]);

		glViewport(0, 0, 300, 200);
		glClearColor(0.2f, 0.4f, 0.8f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glFinish();
		printf("glClear(blue) + glFinish done\n");

		// Read back pixels via GL
		uint8 pixel[4] = {0};
		glReadPixels(150, 100, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
		printf("glReadPixels(150,100): R=%u G=%u B=%u A=%u\n",
			pixel[0], pixel[1], pixel[2], pixel[3]);

		// Also try a simple triangle
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(0, 300, 0, 200, -1, 1);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();

		glBegin(GL_TRIANGLES);
		glColor3f(1.0f, 0.0f, 0.0f);
		glVertex2f(150, 180);
		glColor3f(0.0f, 1.0f, 0.0f);
		glVertex2f(50, 20);
		glColor3f(0.0f, 0.0f, 1.0f);
		glVertex2f(250, 20);
		glEnd();
		glFinish();
		printf("Triangle drawn + glFinish done\n");

		glReadPixels(150, 100, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
		printf("glReadPixels(150,100): R=%u G=%u B=%u A=%u\n",
			pixel[0], pixel[1], pixel[2], pixel[3]);

		fGLView->SwapBuffers();
		fGLView->UnlockGL();

		printf("=== Done ===\n");
	}

private:
	BGLView* fGLView;
};


class GLTestApp : public BApplication {
public:
	GLTestApp() : BApplication("application/x-vnd.gl-test") {}

	void ReadyToRun()
	{
		fWindow = new GLTestWindow();
		fWindow->Show();
		fWindow->QueryGL();
		// Keep running briefly to see window
		snooze(3000000);
		PostMessage(B_QUIT_REQUESTED);
	}

private:
	GLTestWindow* fWindow;
};


int main()
{
	GLTestApp app;
	app.Run();
	return 0;
}
