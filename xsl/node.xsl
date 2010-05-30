<?xml version="1.0"?>
<xsl:stylesheet version="1.0"
  xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
  xmlns="http://www.w3.org/1999/xhtml">

  <xsl:template match="/">
    <html xmlns="http://www.w3.org/1999/xhtml">
      <head><title>Node</title></head>
      <body style="background-color:white">
        <xsl:for-each select="node/*">
          <xsl:apply-templates select="."/>
        </xsl:for-each>
      </body>
    </html>
  </xsl:template>

  <xsl:template match="*">
  <text style="font-family:Verdana, Arial; font-size:12pt">
    <b><xsl:value-of select="name()"/></b>: <xsl:value-of select="."/>
   <br />
  </text>
  </xsl:template>

  <xsl:template match="subnodesDbUrl">
    <xsl:variable name="subnodesDbUrl" select="."/>
    <xsl:apply-templates select="instance"/>
    <text style="font-family:Verdana, Arial; font-size:12pt">
      <b><xsl:value-of select="name()"/></b>: <a href="{$subnodesDbUrl}?format=xml"><xsl:value-of select="."/></a>
      (<a href="{$subnodesDbUrl}/../../../gridfactory/">info</a>)
      <br />
    </text>
  </xsl:template>

</xsl:stylesheet>
