<?xml version="1.0"?>
<xsl:stylesheet version="1.0"
  xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
  xmlns="http://www.w3.org/1999/xhtml">
  <xsl:template match="/">
    <html xmlns="http://www.w3.org/1999/xhtml">
      <head><title>Nodes</title></head>
      <body style="background-color:white">
      
      <xsl:for-each select="nodes/node">
        <div style="width:450px; padding:5px; margin-bottom:10px;
        border:5px double black; color:black; background-color:white;
        text-align:left">
          <xsl:apply-templates select="."/>
        </div>
      </xsl:for-each>

    </body>
  </html>
</xsl:template>

<xsl:template match="node/*">
  <text style="font-family:Verdana, Arial; font-size:12pt">
    <b><xsl:value-of select="name()"/></b>: <xsl:value-of select="."/>
   <br />
  </text>
</xsl:template>

<xsl:template match="node/subnodesDbUrl">
  <xsl:variable name="subnodesDbUrl" select="."/>
  <xsl:apply-templates select="instance"/>
  <text style="font-family:Verdana, Arial; font-size:12pt">
    <b><xsl:value-of select="name()"/></b>: <a href="{$subnodesDbUrl}?format=xml"><xsl:value-of select="."/></a>
  </text>
  <br />
</xsl:template>

<xsl:template match="node/dbUrl">
  <xsl:variable name="dbUrl" select="."/>
  <xsl:apply-templates select="instance"/>
  <text style="font-family:Verdana, Arial; font-size:12pt">
    <b><xsl:value-of select="name()"/></b>: <a href="{$dbUrl}?format=xml"><xsl:value-of select="."/></a>
  </text>
</xsl:template>

</xsl:stylesheet>
