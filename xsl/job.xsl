<?xml version="1.0"?>
<xsl:stylesheet version="1.0"
  xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
  xmlns="http://www.w3.org/1999/xhtml">

  <xsl:template match="/">
    <html xmlns="http://www.w3.org/1999/xhtml">
      <head><title>Job</title></head>
      <body style="background-color:white">
       <xsl:for-each select="job/*">
         <xsl:apply-templates select="."/>
          <xsl:for-each select="inputFileURL">
            <xsl:apply-templates select="."/>
          </xsl:for-each>
          <xsl:for-each select="source|destination">
            <xsl:apply-templates select="."/>
          </xsl:for-each>
       <br />
       </xsl:for-each>
      </body>
    </html>
  </xsl:template>

  <xsl:template match="*">
  <text style="font-family:Verdana, Arial; font-size:12pt">
    <b><xsl:value-of select="name()"/></b>: <xsl:value-of select="."/>
  </text>
  </xsl:template>

<xsl:template match="dbUrl">
  <xsl:variable name="dbUrl" select="."/>
  <text style="font-family:Verdana, Arial; font-size:12pt">
    <b><xsl:value-of select="name()"/></b>: <a href="{$dbUrl}?format=xml"><xsl:value-of select="."/></a>
    <br />
  </text>
</xsl:template>

<xsl:template match="identifier">
  <xsl:variable name="identifier" select="."/>
  <text style="font-family:Verdana, Arial; font-size:12pt">
    <b><xsl:value-of select="name()"/></b> (job directory): <a href="{$identifier}?format=xml"><xsl:value-of select="."/></a>
  </text>
</xsl:template>

<xsl:template match="*[starts-with(name(), 'std')]">
  <xsl:variable name="link" select="."/>
  <text style="font-family:Verdana, Arial; font-size:12pt">
    <b><xsl:value-of select="name()"/></b>: <a href="{$link}"><xsl:value-of select="."/></a>
  </text>
</xsl:template>

<xsl:template match="inputFileURL">
  <xsl:variable name="link" select="."/>
  <text style="font-family:Verdana, Arial; font-size:12pt">
      <xsl:text> </xsl:text><a href="{$link}"><xsl:value-of select="."/></a>
  </text>
</xsl:template>

<xsl:template match="inputFileURLs|outFileMapping">
  <xsl:variable name="link" select="."/>
  <text style="font-family:Verdana, Arial; font-size:12pt">
      <b><xsl:value-of select="name()"/></b>:
  </text>
</xsl:template>

<xsl:template match="source">
  <xsl:variable name="link" select="."/>
  <text style="font-family:Verdana, Arial; font-size:12pt">
      <xsl:text> </xsl:text><xsl:value-of select="."/><xsl:text> &#8594; </xsl:text>
  </text>
</xsl:template>

<xsl:template match="destination">
  <xsl:variable name="link" select="."/>
  <text style="font-family:Verdana, Arial; font-size:12pt">
      <xsl:text> </xsl:text><a href="{$link}"><xsl:value-of select="."/></a><xsl:text>&#160;&#160;</xsl:text>
  </text>
</xsl:template>

</xsl:stylesheet>
