/*
 * Wolfenstein: Enemy Territory GPL Source Code
 * Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company.
 *
 * ET: Legacy
 * Copyright (C) 2012-2018 ET:Legacy team <mail@etlegacy.com>
 *
 * This file is part of ET: Legacy - http://www.etlegacy.com
 *
 * ET: Legacy is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ET: Legacy is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ET: Legacy. If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, Wolfenstein: Enemy Territory GPL Source Code is also
 * subject to certain additional terms. You should have received a copy
 * of these additional terms immediately following the terms and conditions
 * of the GNU General Public License which accompanied the source code.
 * If not, please request a copy in writing from id Software at the address below.
 *
 * id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.
 */
/**
 * @file renderer/tr_fbo.c
 * @brief framebuffer object handling for r1
 */

#include "tr_local.h"

frameBuffer_t *mainFbo;
frameBuffer_t *msMainFbo;

static frameBuffer_t *current;

#define MAX_FBOS 10
frameBuffer_t systemFbos[MAX_FBOS];

const char *fboBlitVert = "#version 110\n"
						  "void main(void) {\n"
						  "gl_Position = gl_Vertex;\n"
						  "gl_TexCoord[0] = gl_MultiTexCoord0;\n"
						  "}\n";

const char *fboBlitFrag = "#version 110\n"
						  "uniform sampler2D u_CurrentMap;\n"
						  "void main(void) {\n"
						  "gl_FragColor = texture2D(u_CurrentMap, gl_TexCoord[0].st);\n"
						  "}\n";

static shaderProgram_t *blitProgram;

static frameBuffer_t *R_FindAvailableFbo(void)
{
	for (int i = 0; i < MAX_FBOS; i++)
	{
		if (!systemFbos[i].fbo)
		{
			return &systemFbos[i];
		}
	}

	return NULL;
}

static void R_SetWindowViewport(void)
{
	glViewport(0, 0, glConfig.windowWidth, glConfig.windowHeight);
	glScissor(0, 0, glConfig.windowWidth, glConfig.windowHeight);
	glOrtho(0, glConfig.windowWidth, glConfig.windowHeight, 0, 0, 1);
}

static void R_SetFBOViewport(frameBuffer_t *fb)
{
	glViewport(0, 0, fb->width, fb->height);
	glScissor(0, 0, fb->width, fb->height);
	glOrtho(0, fb->width, fb->height, 0, 0, 1);
}

void R_FBOSetViewport(frameBuffer_t *from, frameBuffer_t *to)
{
	if (!tr.useFBO)
	{
		return;
	}

	if (!from && !to)
	{
		return;
	}

	if (from && to)
	{
		if (from == to)
		{
			return;
		}

		if (from->width == to->width && from->height == to->height)
		{
			return;
		}

		R_SetFBOViewport(to);
		return;
	}

	frameBuffer_t *com = NULL;
	if (from)
	{
		com = from;
	}
	else
	{
		com = to;
	}

	if (glConfig.windowHeight != com->height || glConfig.windowWidth != com->width)
	{
		if (to)
		{
			R_SetFBOViewport(to);
		}
		else
		{
			R_SetWindowViewport();
		}
	}
}

void R_BindFBO(frameBuffer_t *fb)
{
	if (!tr.useFBO)
	{
		return;
	}

	current = fb;

	if (fb)
	{
		glBindFramebufferEXT(GL_FRAMEBUFFER, fb->fbo);
	}
	else
	{
		glBindFramebufferEXT(GL_FRAMEBUFFER, 0);
	}
}

frameBuffer_t *R_CurrentFBO()
{
	return current;
}

static void R_BindFBOAs(frameBuffer_t *fb, fboBinding binding)
{
	GLint val = 0;

	switch (binding)
	{
		case READ:
			val = GL_READ_FRAMEBUFFER_EXT;
			break;
		case WRITE:
			val = GL_DRAW_FRAMEBUFFER_EXT;
			break;
		case BOTH:
			val = GL_FRAMEBUFFER_EXT;
			break;
		default:
			Ren_Fatal("Invalid binding type\n");
	}

	if (fb)
	{
		glBindFramebufferEXT(val, fb->fbo);
	}
	else
	{
		glBindFramebufferEXT(val, 0);
	}
}

static void R_GetCurrentFBOId(fboBinding binding, GLint *fboId)
{
	switch (binding)
	{
		case READ:
			glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT, fboId);
			break;
		case WRITE:
			glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_EXT, fboId);
			break;
		case BOTH:
			glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, fboId);
			break;
		default:
			Ren_Fatal("Invalid binding type\n");
	}
}

byte *R_FBOReadPixels(frameBuffer_t *fb, size_t *offset, int *padlen)
{
	GLint currentRead;
	byte* data = NULL;

	if (!tr.useFBO)
	{
		return RB_ReadPixels(0, 0, glConfig.windowWidth, glConfig.windowHeight, offset, padlen);;
	}

	R_GetCurrentFBOId(READ, &currentRead);

	R_BindFBOAs(fb, READ);

	if (fb)
	{
		data = RB_ReadPixels(0, 0, fb->width, fb->height, offset, padlen);
	}
	else
	{
		data = RB_ReadPixels(0, 0, glConfig.windowWidth, glConfig.windowHeight, offset, padlen);
	}

	glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, currentRead);

	return data;
}

static void R_CreateFBODepthAttachment(frameBuffer_t *fb, int samples, int stencilBits, qboolean texture)
{
	// if multisampled have this route..
	if (samples)
	{
		glGenRenderbuffersEXT(1, &fb->depthBuffer);
		glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, fb->depthBuffer);

		if (stencilBits == 0)
		{
			glRenderbufferStorageMultisampleEXT(GL_RENDERBUFFER_EXT, samples, GL_DEPTH_COMPONENT32, fb->width,
												fb->height);
			glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT,
										 fb->depthBuffer);
		}
		else
		{
			glRenderbufferStorageMultisampleEXT(GL_RENDERBUFFER_EXT, samples, GL_DEPTH24_STENCIL8, fb->width,
												fb->height);
			glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER_EXT,
										 fb->depthBuffer);
		}

		return;
	}

	// if we want a texture attachment then this path..
	if (texture)
	{
		glGenTextures(1, &fb->depth);
		glBindTexture(GL_TEXTURE_2D, fb->depth);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

		if (stencilBits == 0)
		{
			glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32, fb->width, fb->height, 0, GL_DEPTH_COMPONENT,
						 GL_FLOAT,
						 NULL);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, fb->depth, 0);
		}
		else
		{
			fb->stencil = qtrue;
			glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, fb->width, fb->height, 0, GL_DEPTH_STENCIL,
						 GL_UNSIGNED_INT_24_8, NULL);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, fb->depth, 0);
		}

		return;
	}

	// otherwise just default to a renderbuffer
	glGenRenderbuffersEXT(1, &fb->depthBuffer);
	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, fb->depthBuffer);

	if (stencilBits == 0)
	{
		glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT32, fb->width, fb->height);
		glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT,
									 fb->depthBuffer);
	}
	else
	{
		glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8, fb->width, fb->height);
		glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER_EXT,
									 fb->depthBuffer);
	}
}

static void R_CreateFBOColorAttachment(frameBuffer_t *fb, int samples)
{
	if (samples)
	{
		glGenRenderbuffersEXT(1, &fb->colorBuffer);
		glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, fb->colorBuffer);
		glRenderbufferStorageMultisampleEXT(GL_RENDERBUFFER_EXT, samples, GL_RGBA8, fb->width, fb->height);
		glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT,
									 fb->colorBuffer);
	}
	else
	{
		glGenTextures(1, &fb->color);
		glBindTexture(GL_TEXTURE_2D, fb->color);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fb->width, fb->height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb->color, 0);
	}
}

static void R_DestroyFBO(frameBuffer_t *fb)
{
	if (!fb || !fb->fbo)
	{
		return;
	}

	R_BindFBO(fb);

	glBindTexture(GL_TEXTURE_2D, 0);

	if (fb->color)
	{
		glDeleteRenderbuffersEXT(1, &fb->color);
	}

	if (fb->depth)
	{
		glDeleteRenderbuffersEXT(1, &fb->depth);
	}

	if (fb->colorBuffer)
	{
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		glDeleteTextures(1, &fb->colorBuffer);
		fb->colorBuffer = 0;
	}

	if (fb->depthBuffer)
	{
		if (fb->stencil)
		{
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
		}
		else
		{
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
		}

		glDeleteTextures(1, &fb->depthBuffer);
		fb->depthBuffer = 0;
	}

	R_BindFBO(NULL);
	glDeleteFramebuffersEXT(1, &fb->fbo);
	fb->fbo = 0;

	Com_Memset(fb, 0, sizeof(frameBuffer_t));
}

static frameBuffer_t *R_CreateFBO(frameBuffer_t *fb, const char *name, int width, int height, int samples, int stencil)
{
	if (!fb)
	{
		fb = R_FindAvailableFbo();
	}

	if (!fb)
	{
		Ren_Fatal("Could not acquire an FBO handle\n");
	}

	if (fb->fbo)
	{
		R_DestroyFBO(fb);
	}

	Com_Memset(fb, 0, sizeof(frameBuffer_t));

	if (name)
	{
		strcpy(fb->name, name);
	}

	fb->width = width;
	fb->height = height;

	if (stencil)
	{
		fb->stencil = qtrue;
	}

	glGenFramebuffersEXT(1, &fb->fbo);
	R_BindFBO(fb);

	if (!GLEW_EXT_framebuffer_multisample)
	{
		samples = 0;
	}
	else
	{
		//We need to find out what the maximum supported samples is
		GLint maxSamples;
		glGetIntegerv(GL_MAX_SAMPLES_EXT, &maxSamples);

		if (samples > maxSamples)
		{
			samples = maxSamples;
		}
	}

	fb->samples = samples;

	// if (qtrue)
	// {
	// 	if (GLEW_EXT_framebuffer_multisample)
	// 	{
	// 		Ren_Fatal("DERP!\n");
	// 	}
	// 	else if (glRenderbufferStorageMultisample)
	// 	{
	// 		Ren_Fatal("Should work..\n");
	// 	}
	// 	else
	// 	{
	// 		Ren_Fatal("Missing render buffer storage multisample..\n");
	// 	}
	// }

	R_CreateFBOColorAttachment(fb, samples);
	R_CreateFBODepthAttachment(fb, samples, stencil, qfalse);

	if (!samples)
	{
		glBindTexture(GL_TEXTURE_2D, 0);
	}


	if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE)
	{
		Ren_Fatal("Failed to init FBO\n");
	}

	R_BindFBO(NULL);

	return fb;
}

void R_FboBlit(frameBuffer_t *from, frameBuffer_t *to)
{
	if (!tr.useFBO)
	{
		return;
	}

	glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, from->fbo);

	if (to)
	{
		glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, to->fbo);
		R_SetFBOViewport(to);
	}
	else
	{
		glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, 0);
		glDrawBuffer(GL_BACK);
		R_SetWindowViewport();
	}

	GL_CheckErrors();

	if (to)
	{
		glBlitFramebuffer(0, 0, from->width, from->height, 0, 0, to->width, to->height,
						  GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	}
	else
	{
		glBlitFramebuffer(0, 0, from->width, from->height, 0, 0, glConfig.windowWidth, glConfig.windowHeight,
						  GL_COLOR_BUFFER_BIT, GL_LINEAR);
	}

	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, to->fbo);

	GL_CheckErrors();
}

/*void R_MainFBOBlit(void)
{
	if (!mainFbo.fbo)
	{
		return;
	}

	// GLint target;
	// glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &target);

	glBindFramebuffer(GL_FRAMEBUFFER, mainFbo.fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	// glReadBuffer(GL_COLOR_ATTACHMENT0);
	// glDrawBuffer(GL_FRONT);
	glDrawBuffer(GL_BACK);

	R_SetWindowViewport();

	GL_CheckErrors();

	if (blitProgram)
	{
		glActiveTextureARB(GL_TEXTURE0_ARB);
		glClientActiveTextureARB(GL_TEXTURE0_ARB);
		glBindTexture(GL_TEXTURE_2D, mainFbo.color);

		glBegin(GL_QUADS);
		{
			glTexCoord2f(0.0f, 0.0f);
			glVertex3f(-1.0f, -1.0f, 0.0f);
			glTexCoord2f(1.0f, 0.0f);
			glVertex3f(1.0f, -1.0f, 0.0f);
			glTexCoord2f(1.0f, 1.0f);
			glVertex3f(1.0f, 1.0f, 0.0f);
			glTexCoord2f(0.0f, 1.0f);
			glVertex3f(-1.0f, 1.0f, 0.0f);
		}
		glEnd();
	}
	else
	{
		glBlitFramebuffer(0, 0, mainFbo.width, mainFbo.height, 0, 0, glConfig.windowWidth, glConfig.windowHeight,
						  GL_COLOR_BUFFER_BIT, GL_LINEAR);
	}

	GL_CheckErrors();

	// glBindFramebuffer(GL_FRAMEBUFFER, target);
	R_BindFBO(current);
}*/

void R_InitFBO(void)
{
	Com_Memset(&systemFbos, 0, sizeof(frameBuffer_t) * MAX_FBOS);

	current = NULL;
	blitProgram = NULL;

	if (!r_fbo->integer)
	{
		return;
	}

	if (!GLEW_ARB_framebuffer_object)
	{
		Ren_Print("WARNING: R_InitFBO() skipped - no GLEW_ARB_framebuffer_object\n");
		return;
	}

	tr.useFBO = qtrue;

	Ren_Print("Setting up FBO\n");

	mainFbo = NULL;
	msMainFbo = NULL;

	int samples = ri.Cvar_VariableIntegerValue("r_ext_multisample");
	int stencil = ri.Cvar_VariableIntegerValue("r_stencilbits");

	GL_CheckErrors();

	if (samples)
	{
		msMainFbo = R_CreateFBO(NULL, "multisampled-main", glConfig.vidWidth, glConfig.vidHeight, samples, stencil);
		mainFbo = R_CreateFBO(NULL, "main", glConfig.vidWidth, glConfig.vidHeight, 0, stencil);
	}
	else
	{
		mainFbo = R_CreateFBO(NULL, "main", glConfig.vidWidth, glConfig.vidHeight, samples, stencil);
	}

	// Setup the blitting program
	// blitProgram = R_CreateShaderProgram(fboBlitVert, fboBlitFrag);
	// R_UseShaderProgram(blitProgram);
	// GLint blitProgramMap = R_GetShaderProgramUniform(blitProgram, "u_CurrentMap");
	// glUniform1i(blitProgramMap, 0);
	//
	// R_UseShaderProgram(NULL);

	GL_CheckErrors();
}

void R_ShutdownFBO(void)
{
	if (!tr.useFBO)
	{
		return;
	}

	for (int i = 0; i < MAX_FBOS; i++)
	{
		R_DestroyFBO(&systemFbos[i]);
	}

	Com_Memset(&systemFbos, 0, sizeof(frameBuffer_t) * MAX_FBOS);

	blitProgram = NULL;
}