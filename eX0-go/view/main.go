package main

import (
	"github.com/go-gl/mathgl/mgl32"
	"github.com/shurcooL/gogl"
	glfw "github.com/shurcooL/goglfw"
)

var gl *gogl.Context

var windowSize = [2]int{1024, 1024}

var cameraX, cameraY float64 = 825, 510

func main() {
	err := glfw.Init()
	if err != nil {
		panic(err)
	}
	defer glfw.Terminate()

	glfw.WindowHint(glfw.Samples, 8) // Anti-aliasing.

	window, err := glfw.CreateWindow(windowSize[0], windowSize[1], "", nil, nil)
	if err != nil {
		panic(err)
	}
	window.MakeContextCurrent()

	gl = window.Context

	gl.ClearColor(0.600, 0.741, 0.565, 1)
	gl.Clear(gl.COLOR_BUFFER_BIT)

	window.SetScrollCallback(func(_ *glfw.Window, xoff, yoff float64) {
		cameraX += xoff * 5
		cameraY += yoff * 5
	})

	framebufferSizeCallback := func(w *glfw.Window, framebufferSize0, framebufferSize1 int) {
		gl.Viewport(0, 0, framebufferSize0, framebufferSize1)

		windowSize[0], windowSize[1] = w.GetSize()
	}
	{
		var framebufferSize [2]int
		framebufferSize[0], framebufferSize[1] = window.GetFramebufferSize()
		framebufferSizeCallback(window, framebufferSize[0], framebufferSize[1])
	}
	window.SetFramebufferSizeCallback(framebufferSizeCallback)

	l, err := newLevel("../../eX0/levels/test3.wwl")
	if err != nil {
		panic(err)
	}

	c, err := newCharacter()
	if err != nil {
		panic(err)
	}

	for !window.ShouldClose() {
		gl.Clear(gl.COLOR_BUFFER_BIT)

		pMatrix := mgl32.Ortho2D(0, float32(windowSize[0]), float32(windowSize[1]), 0)
		mvMatrix := mgl32.Translate3D(float32(cameraX), float32(cameraY), 0)

		l.setup()
		gl.UniformMatrix4fv(l.pMatrixUniform, false, pMatrix[:])
		gl.UniformMatrix4fv(l.mvMatrixUniform, false, mvMatrix[:])
		l.render()

		c.setup()
		gl.UniformMatrix4fv(c.pMatrixUniform, false, pMatrix[:])
		gl.UniformMatrix4fv(c.mvMatrixUniform, false, mvMatrix[:])
		c.render()

		window.SwapBuffers()
		glfw.PollEvents()
	}
}