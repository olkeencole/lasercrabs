#include "model.h"

Model::Data::Data()
	: attribs()
{
	glGenVertexArrays(1, &gl_vertex_array);
	glBindVertexArray(gl_vertex_array);
	glGenBuffers(1, &gl_index_buffer);
}

Model::Data::~Data()
{
	for (int i = 0; i < attribs.length; i++)
		glDeleteBuffers(1, &attribs.data[i].gl_buffer);
	glDeleteVertexArrays(1, &gl_vertex_array);
}

void Model::Data::set_indices(Array<int>& indices)
{
	glBindVertexArray(gl_vertex_array);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl_index_buffer);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.length * sizeof(int), indices.data, GL_STATIC_DRAW);
	index_count = indices.length;
}

void Model::Data::bind()
{
	for (int i = 0; i < attribs.length; i++)
	{
		glEnableVertexAttribArray(i);
		glBindBuffer(GL_ARRAY_BUFFER, attribs.data[i].gl_buffer);
		glVertexAttribPointer(
			i,                         // attribute
			attribs.data[i].element_size, // size
			attribs.data[i].type,         // type
			GL_FALSE,                  // normalized?
			0,                         // stride
			(void*)0                   // array buffer offset
		);
	}

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl_index_buffer);
}

void Model::Data::unbind()
{
	for (int i = 0; i < attribs.length; i++)
		glDisableVertexAttribArray(i);
}

void Model::exec(RenderParams* params)
{
	if (data)
	{
		data->bind();
		// Draw the triangles !
		glDrawElements(
			GL_TRIANGLES,      // mode
			data->index_count,       // count
			GL_UNSIGNED_INT,   // type
			(void*)0           // element array buffer offset
		);
		data->unbind();
	}
}